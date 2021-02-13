/*
Copyright 2020 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include "common.h"
#include "sample.h"
#include "fuzzer.h"
#include "sampledelivery.h"
#include "instrumentation.h"
#include "coverage.h"
#include "mutator.h"
#include "thread.h"
#include "directory.h"
#include "client.h"
#include "mersenne.h"

using namespace std;

void Fuzzer::PrintUsage() {
  printf("Incorrect usage, please refer to the doumentation\n");
  exit(0);
}

void Fuzzer::ParseOptions(int argc, char **argv) {
  save_hangs = false;
  server_update_interval_ms = 5 * 60 * 1000;
  acceptable_hang_ratio = 0.01;
  acceptable_crash_ratio = 0.02;

  num_threads = 1;

  char *option;

  option = GetOption("-in", argc, argv);
  if (!option) PrintUsage();
  this->in_dir = option;

  option = GetOption("-out", argc, argv);
  if (!option) PrintUsage();
  this->out_dir = option;

  num_threads = GetIntOption("-nthreads", argc, argv, 1);

  int target_opt_ind = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--") == 0) {
      target_opt_ind = i + 1;
      break;
    }
  }

  char *cmd = NULL;
  if (target_opt_ind) {
    target_argc = argc - target_opt_ind;
    target_argv = argv + target_opt_ind;
  } else {
    target_argc = 0;
    target_argv = NULL;
  }

  timeout = GetIntOption("-t", argc, argv, 0x7FFFFFFF);

  init_timeout = GetIntOption("-t1", argc, argv, timeout);
  
  corpus_timeout = GetIntOption("-t_corpus", argc, argv, timeout);

  if (GetOption("-server", argc, argv)) {
    server = new CoverageClient();
    server->Init(argc, argv);
  } else {
    server = NULL;
  }
  
  should_restore_state = false;
  if((in_dir == "-") ||
     GetBinaryOption("-restore", argc, argv, false) ||
     GetBinaryOption("-resume", argc, argv, false))
  {
    should_restore_state = true;
  }
}

void Fuzzer::SetupDirectories() {
  //create output directories
  CreateDirectory(out_dir);
  crash_dir = DirJoin(out_dir, "crashes");
  CreateDirectory(crash_dir);
  hangs_dir = DirJoin(out_dir, "hangs");
  CreateDirectory(hangs_dir);
  sample_dir = DirJoin(out_dir, "samples");
  CreateDirectory(sample_dir);
}

void *StartFuzzThread(void *arg) {
  Fuzzer::ThreadContext *tc = (Fuzzer::ThreadContext*)arg;
  tc->fuzzer->RunFuzzerThread(tc);
  return NULL;
}

Fuzzer::ThreadContext::~ThreadContext() {
  if (sampleDelivery) delete sampleDelivery;
  if (prng) delete prng;
  if (mutator) delete mutator;
  if (instrumentation) delete instrumentation;
  if (target_argv) free(target_argv);
}

void Fuzzer::Run(int argc, char **argv) {
  if (GetOption("-start_server", argc, argv)) {
    // run the server
    printf("Running as server\n");
    CoverageServer server;
    server.Init(argc, argv);
    server.RunServer();
    return;
  }

  printf("Fuzzer version 0.01\n");

  samples_pending = 0;
  
  num_crashes = 0;
  num_unique_crashes = 0;
  num_hangs = 0;
  num_samples = 0;
  num_samples_discarded = 0;
  total_execs = 0;
  min_priority = 1.79e+308;

  ParseOptions(argc, argv);

  SetupDirectories();

  if(should_restore_state) {
    RestoreState();
  } else {
    GetFilesInDirectory(in_dir, input_files);

    if (input_files.size() == 0) {
      FATAL("Error: no input files read\n");
    } else {
      SAY("%d input files read\n", (int)input_files.size());
    }
  }

  // in case of state restoring,
  // input_files is empty, so this is fine
  state = INPUT_SAMPLE_PROCESSING;
  
  for (int i = 1; i <= num_threads; i++) {
    ThreadContext *tc = CreateThreadContext(argc, argv, i);
    CreateThread(StartFuzzThread, tc);
  }

  uint64_t last_execs = 0;
  
  uint32_t secs_to_sleep = 1;
  
  uint64_t secs_since_last_save = 0;
  
  while (1) {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
    Sleep(secs_to_sleep * 1000);
#else
    usleep(secs_to_sleep * 1000000);
#endif
    
    secs_since_last_save += secs_to_sleep;
    if(secs_since_last_save >= FUZZER_SAVE_INERVAL) {
      SaveState();
      secs_since_last_save = 0;
    }
    
    size_t num_offsets = 0;
    coverage_mutex.Lock();
    for(auto iter = fuzzer_coverage.begin(); iter != fuzzer_coverage.end(); iter++) {
      num_offsets += iter->offsets.size();
    }
    coverage_mutex.Unlock();
    
    printf("\nTotal execs: %lld\nUnique samples: %lld (%lld discarded)\nCrashes: %lld (%lld unique)\nHangs: %lld\nOffsets: %zu\nExecs/s: %lld\n", total_execs, num_samples, num_samples_discarded, num_crashes, num_unique_crashes, num_hangs, num_offsets, (total_execs - last_execs) / secs_to_sleep);
    last_execs = total_execs;
  }
}

RunResult Fuzzer::RunSampleAndGetCoverage(ThreadContext *tc, Sample *sample, Coverage *coverage, uint32_t init_timeout, uint32_t timeout) {
  // not protected by a mutex but not important to be perfectly accurate
  total_execs++;

  if (!tc->sampleDelivery->DeliverSample(sample)) {
    WARN("Error delivering sample, retrying with a clean target");
    tc->instrumentation->CleanTarget();
    if (!tc->sampleDelivery->DeliverSample(sample)) {
      FATAL("Repeatedly failed to deliver sample");
    }
  }

  RunResult result = tc->instrumentation->Run(tc->target_argc, tc->target_argv, init_timeout, timeout);
  tc->instrumentation->GetCoverage(*coverage, true);

  // save crashes and hangs immediately when they are detected
  if (result == CRASH) {
    string crash_desc = tc->instrumentation->GetCrashName();

    if (TryReproduceCrash(tc, sample, init_timeout, timeout) == CRASH) {
      // get a hopefully better name
      crash_desc = tc->instrumentation->GetCrashName();
    } else {
      crash_desc = "flaky_" + crash_desc;
    }
    
    bool should_save_crash = false;
    int duplicates = 0;
    
    crash_mutex.Lock();
    num_crashes++;

    auto crash_it = unique_crashes.find(crash_desc);
    if(crash_it == unique_crashes.end()) {
      should_save_crash = true;
      duplicates = 1;
      unique_crashes[crash_desc] = 1;
      num_unique_crashes++;
    } else {
      if(crash_it->second < MAX_IDENTICAL_CRASHES) {
        should_save_crash = true;
        crash_it->second++;
        duplicates = crash_it->second;
      }
    }
    crash_mutex.Unlock();

    if(should_save_crash) {
      string crash_filename = crash_desc + "_" + std::to_string(duplicates);
      
      output_mutex.Lock();
      string outfile = DirJoin(crash_dir, crash_filename);
      sample->Save(outfile.c_str());
      output_mutex.Unlock();

      if (server) {
        server_mutex.Lock();
        server->ReportCrash(sample, crash_desc);
        server_mutex.Unlock();
      }
    }
  }

  if (result == HANG) {
    output_mutex.Lock();
    if (save_hangs) {
      string outfile = DirJoin(hangs_dir, string("hang_") + std::to_string(num_hangs));
      sample->Save(outfile.c_str());
    }
    num_hangs++;
    output_mutex.Unlock();
  }

  return result;
}

RunResult Fuzzer::TryReproduceCrash(ThreadContext* tc, Sample* sample, uint32_t init_timeout, uint32_t timeout) {
  RunResult result;

  for (int i = 0; i < CRASH_REPRODUCE_TIMES; i++) {
    total_execs++;

    if (!tc->sampleDelivery->DeliverSample(sample)) {
      WARN("Error delivering sample, retrying with a clean target");
      tc->instrumentation->CleanTarget();
      if (!tc->sampleDelivery->DeliverSample(sample)) {
        FATAL("Repeatedly failed to deliver sample");
      }
    }

    result = tc->instrumentation->RunWithCrashAnalysis(tc->target_argc, tc->target_argv, init_timeout, timeout);
    tc->instrumentation->ClearCoverage();

    if (result == CRASH) return result;
  }

  return result;
}

RunResult Fuzzer::RunSample(ThreadContext *tc, Sample *sample, int *has_new_coverage, bool trim, bool report_to_server, uint32_t init_timeout, uint32_t timeout) {
  Sample filteredSample;
  if (OutputFilter(sample, &filteredSample)) {
    sample = &filteredSample;
  }

  if (has_new_coverage) {
    *has_new_coverage = 0;
  }

  Coverage initialCoverage;

  RunResult result = RunSampleAndGetCoverage(tc, sample, &initialCoverage, init_timeout, timeout);

  if (result != OK) return result;

  if (initialCoverage.empty()) return result;

  // printf("found new coverage: \n");
  // PrintCoverage(initialCoverage);

  // the sample returned new coverage

  Coverage stableCoverage = initialCoverage;
  Coverage totalCoverage = initialCoverage;

  // have a clean target before retrying the sample
  tc->instrumentation->CleanTarget();

  for (int i = 0; i < SAMPLE_RETRY_TIMES; i++) {
    Coverage retryCoverage, tmpCoverage;

    result = RunSampleAndGetCoverage(tc, sample, &retryCoverage, init_timeout, timeout);
    if (result != OK) return result;

    // printf("Retry %d, coverage:\n", i);
    // PrintCoverage(retryCoverage);

    MergeCoverage(totalCoverage, retryCoverage);
    CoverageIntersection(stableCoverage, retryCoverage, tmpCoverage);

    stableCoverage = tmpCoverage;
  }

  Coverage variableCoverage;
  CoverageDifference(stableCoverage, totalCoverage, variableCoverage);

  // printf("Stable coverage:\n");
  // PrintCoverage(stableCoverage);
  // printf("Variable coverage:\n");
  // PrintCoverage(variableCoverage);

  if (InterestingSample(tc, sample, &stableCoverage, &variableCoverage)) {
    if (has_new_coverage) {
      *has_new_coverage = 1;
    }

    if (trim) TrimSample(tc, sample, &stableCoverage, init_timeout, timeout);

    output_mutex.Lock();
    char fileindex[20];
    sprintf(fileindex, "%05lld", num_samples);
    string outfile = DirJoin(sample_dir, string("sample_") + fileindex);
    sample->Save(outfile.c_str());
    num_samples++;
    output_mutex.Unlock();

    if (server && report_to_server) {
      server_mutex.Lock();
      server->ReportNewCoverage(&stableCoverage, sample);
      server_mutex.Unlock();
    }

    SampleQueueEntry *new_entry = new SampleQueueEntry();
    Sample *new_sample = new Sample(*sample);
    new_entry->sample = new_sample;
    new_entry->context = tc->mutator->CreateSampleContext(new_entry->sample);
    new_entry->context_initialized = true;
    new_entry->priority = 0;
    new_entry->sample_index = num_samples - 1;

    queue_mutex.Lock();
    all_samples.push_back(new_sample);
    sample_queue.push(new_entry);
    queue_mutex.Unlock();
  } 
  
  if (!variableCoverage.empty() && server && report_to_server) {
    server_mutex.Lock();
    server->ReportNewCoverage(&variableCoverage, NULL);
    server_mutex.Unlock();
  }

  // printf("Total coverage:\n");
  // PrintCoverage(totalCoverage);

  tc->instrumentation->IgnoreCoverage(totalCoverage);

  return result;
}

void Fuzzer::TrimSample(ThreadContext *tc, Sample *sample, Coverage* stable_coverage, uint32_t init_timeout, uint32_t timeout) {
  if (sample->size <= 1) return;

  int trim_step = TRIM_STEP_INITIAL;

  size_t trimmed_size = sample->size;
  Sample test_sample = *sample;

  while (1) {
    if (test_sample.size <= 1) break;
    while (trim_step >= test_sample.size) {
      trim_step /= 2;
    }
    if (trim_step == 0) break;

    test_sample.Trim(test_sample.size - trim_step);

    Coverage test_coverage;

    RunResult result = RunSampleAndGetCoverage(tc, &test_sample, &test_coverage, init_timeout, timeout);
    if (result != OK) break;

    if (!CoverageContains(test_coverage, *stable_coverage)) {
      trim_step /= 2;
      if (trim_step == 0) break;

      test_sample = *sample;
      test_sample.Trim(trimmed_size);
      continue;
    }

    trimmed_size = test_sample.size;
  }

  if (trimmed_size < sample->size) {
    sample->Trim(trimmed_size);
  }
}


int Fuzzer::InterestingSample(ThreadContext *tc, Sample *sample, Coverage *stableCoverage, Coverage *variableCoverage) {
  coverage_mutex.Lock();

  Coverage new_stable_coverage;
  Coverage new_variable_coverage;

  CoverageDifference(fuzzer_coverage, *stableCoverage, new_stable_coverage);
  CoverageDifference(fuzzer_coverage, *variableCoverage, new_variable_coverage);

  MergeCoverage(fuzzer_coverage, new_stable_coverage);
  MergeCoverage(fuzzer_coverage, new_variable_coverage);

  coverage_mutex.Unlock();

  // printf("New stable coverage:\n");
  // PrintCoverage(new_stable_coverage);
  // printf("New variable coverage:\n");
  // PrintCoverage(new_variable_coverage);

  *stableCoverage = new_stable_coverage;
  *variableCoverage = new_variable_coverage;

  if (!new_stable_coverage.empty()) return 1;

  return 0;
}

void Fuzzer::SynchronizeAndGetJob(ThreadContext* tc, FuzzerJob* job) {
  queue_mutex.Lock();

  // sync all_samples_local with all_samples
  if (all_samples.size() > tc->all_samples_local.size()) {
    size_t old_size = tc->all_samples_local.size();
    tc->all_samples_local.resize(all_samples.size());
    for (size_t i = old_size; i < all_samples.size(); i++) {
      tc->all_samples_local[i] = all_samples[i];
    }
  }

  // change state if needed

  if ((state == FUZZING) && server &&
    (GetCurTime() > (last_server_update_time_ms + server_update_interval_ms)))
  {
    last_server_update_time_ms = GetCurTime();
    server_mutex.Lock();
    server->GetUpdates(&server_samples, total_execs);
    server_mutex.Unlock();
    state = SERVER_SAMPLE_PROCESSING;
  }

  if (state == INPUT_SAMPLE_PROCESSING) {
    if (input_files.empty() && !samples_pending) {
      if (sample_queue.empty()) {
        FATAL("No interesting input files\n");
      }
      if (server) {
        server_mutex.Lock();
        coverage_mutex.Lock();
        server->ReportNewCoverage(&fuzzer_coverage, NULL);
        coverage_mutex.Unlock();
        last_server_update_time_ms = GetCurTime();
        server->GetUpdates(&server_samples, total_execs);
        server_mutex.Unlock();
        state = SERVER_SAMPLE_PROCESSING;
      } else {
        state = FUZZING;
      }
    }
  }
  
  if (state == SERVER_SAMPLE_PROCESSING) {
    if (server_samples.empty() && !samples_pending) {
      state = FUZZING;
    }
  }

  if (state == FUZZING) {
    if (sample_queue.empty()) {
      job->type = WAIT;
    } else {
      job->type = FUZZ;
      job->entry = sample_queue.top();
      sample_queue.pop();
      if(job->entry->priority < min_priority) {
        min_priority = job->entry->priority;
      }
    }
  } else if (state == INPUT_SAMPLE_PROCESSING) {
    if (input_files.empty()) {
      job->type = WAIT;
    } else {
      job->type = PROCESS_SAMPLE;
      std::string filename = input_files.front();
      input_files.pop_front();
      printf("Running input sample %s\n", filename.c_str());
      job->sample = new Sample();
      job->sample->Load(filename.c_str());
      if (job->sample->size > MAX_SAMPLE_SIZE) {
        WARN("Input sample larger than maximum sample size. Will be trimmed");
        job->sample->Trim(MAX_SAMPLE_SIZE);
      }
      samples_pending++;
    }
  } else if (state == SERVER_SAMPLE_PROCESSING) {
    if (server_samples.empty()) {
      job->type = WAIT;
    } else {
      job->type = PROCESS_SAMPLE;
      job->sample = new Sample();
      *job->sample = server_samples.front();
      server_samples.pop_front();
      samples_pending++;
    }
  } else {
    job->type = WAIT;
  }

  queue_mutex.Unlock();
}

void Fuzzer::JobDone(FuzzerJob* job) {
  queue_mutex.Lock();

  if (job->type == FUZZ) {
    if (job->discard_sample) {
      delete job->entry;
      num_samples_discarded++;
    } else {
      sample_queue.push(job->entry);
    }
  } else if (job->type == PROCESS_SAMPLE) {
    delete job->sample;
    samples_pending--;
  }

  queue_mutex.Unlock();
}

void Fuzzer::FuzzJob(ThreadContext* tc, FuzzerJob* job) {
  SampleQueueEntry* entry = job->entry;
  
  if(!entry->context_initialized) {
    entry->context = tc->mutator->CreateSampleContext(entry->sample);
    entry->context_initialized = true;
  }

  tc->mutator->InitRound(entry->sample, entry->context);

  printf("Fuzzing sample %05lld\n", entry->sample_index);

  job->discard_sample = false;

  while (1) {
    Sample mutated_sample = *entry->sample;
    if (!tc->mutator->Mutate(&mutated_sample, tc->prng, tc->all_samples_local)) break;
    if (mutated_sample.size > MAX_SAMPLE_SIZE) {
      mutated_sample.Trim(MAX_SAMPLE_SIZE);
    }

    int has_new_coverage;
    RunResult result = RunSample(tc, &mutated_sample, &has_new_coverage, true, true, init_timeout, timeout);
    AdjustSamplePriority(tc, entry, has_new_coverage);
    tc->mutator->NotifyResult(result, has_new_coverage);

    entry->num_runs++;
    if (has_new_coverage) entry->num_newcoverage++;
    if (result == HANG) entry->num_hangs++;
    if (result == CRASH) entry->num_crashes++;
    if ((entry->num_hangs > 10) &&
      (entry->num_hangs > (entry->num_runs * acceptable_hang_ratio)))
    {
      WARN("Sample %lld produces too many hangs. Discarding\n", entry->sample_index);
      job->discard_sample = true;
      break;
    }
    if ((entry->num_crashes > 100) &&
      (entry->num_crashes > (entry->num_runs * acceptable_crash_ratio)))
    {
      WARN("Sample %lld produces too many crashes. Discarding\n", entry->sample_index);
      job->discard_sample = true;
      break;
    }
  }
}


void Fuzzer::RunFuzzerThread(ThreadContext *tc) {
  while (1) {
    FuzzerJob job;

    SynchronizeAndGetJob(tc, &job);

    switch (job.type) {
    case WAIT:
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
      Sleep(1000);
#else
      usleep(1000000);
#endif
      break;
    case PROCESS_SAMPLE:
      RunSample(tc, job.sample, NULL, false, false, init_timeout, corpus_timeout);
      break;
    case FUZZ:
      FuzzJob(tc, &job);
      break;
    default:
      FATAL("Unknown job type");
      break;
    }

    JobDone(&job);
  }
}

void Fuzzer::SaveState() {
  // don't save during input sample processing
  if(state == INPUT_SAMPLE_PROCESSING) return;
  
  output_mutex.Lock();
  coverage_mutex.Lock();
  
  std::string out_file = DirJoin(out_dir, std::string("state.dat"));
  FILE *fp = fopen(out_file.c_str(), "wb");
  if (!fp) {
    FATAL("Error saving state");
  }

  fwrite(&num_samples, sizeof(num_samples), 1, fp);
  fwrite(&total_execs, sizeof(total_execs), 1, fp);
  fwrite(&min_priority, sizeof(min_priority), 1, fp);

  WriteCoverageBinary(fuzzer_coverage, fp);

  fclose(fp);

  coverage_mutex.Unlock();
  output_mutex.Unlock();
}

void Fuzzer::RestoreState() {
  output_mutex.Lock();
  coverage_mutex.Lock();
  queue_mutex.Lock();
  
  std::string out_file = DirJoin(out_dir, std::string("state.dat"));
  FILE *fp = fopen(out_file.c_str(), "rb");
  if (!fp) {
    FATAL("Error restoring state. Did the previous session run long enough for state to be saved?");
  }

  fread(&num_samples, sizeof(num_samples), 1, fp);
  fread(&total_execs, sizeof(total_execs), 1, fp);
  fread(&min_priority, sizeof(min_priority), 1, fp);

  ReadCoverageBinary(fuzzer_coverage, fp);

  fclose(fp);
  
  for (uint64_t i = 0; i < num_samples; i++) {
    Sample *sample = new Sample();
    char fileindex[20];
    sprintf(fileindex, "%05lld", i);
    string outfile = DirJoin(sample_dir, string("sample_") + fileindex);
    sample->Load(outfile.c_str());
    SampleQueueEntry *new_entry = new SampleQueueEntry();
    new_entry->sample = sample;
    new_entry->context = NULL;
    new_entry->context_initialized = false;
    // we don't save priorities per-sample so this is an approximation
    new_entry->priority = min_priority;
    new_entry->sample_index = i;
    all_samples.push_back(sample);
    sample_queue.push(new_entry);
  }
  
  queue_mutex.Unlock();
  coverage_mutex.Unlock();
  output_mutex.Unlock();
}

void Fuzzer::AdjustSamplePriority(ThreadContext *tc, SampleQueueEntry *entry, int found_new_coverage) {
  if (found_new_coverage) entry->priority = 0;
  else entry->priority--;
}

Fuzzer::ThreadContext *Fuzzer::CreateThreadContext(int argc, char **argv, int thread_id) {
  ThreadContext *tc = new ThreadContext();

  // copy arguments for each thread
  tc->target_argc = target_argc;
  tc->target_argv = (char **)malloc((target_argc + 1) * sizeof(char *));
  for (int i = 0; i < target_argc; i++) {
    tc->target_argv[i] = target_argv[i];
  }
  tc->target_argv[target_argc] = NULL;

  tc->thread_id = thread_id;
  tc->fuzzer = this;
  tc->prng = CreatePRNG(argc, argv, tc);
  tc->mutator = CreateMutator(argc, argv, tc);
  tc->instrumentation = CreateInstrumentation(argc, argv, tc);
  tc->sampleDelivery = CreateSampleDelivery(argc, argv, tc);

  // ignore coverage from the corpus
  coverage_mutex.Lock();
  tc->instrumentation->IgnoreCoverage(fuzzer_coverage);
  coverage_mutex.Unlock();

  return tc;
}

bool Fuzzer::MagicOutputFilter(Sample *original_sample, Sample *output_sample, const char *magic, size_t magic_size) {
  if((original_sample->size >= magic_size) && !memcmp(original_sample->bytes, magic, magic_size)) {
    return false;
  }
  
  *output_sample = *original_sample;
  for(int i = 0; i < magic_size; i++) {
    if(i >= output_sample->size) break;
    output_sample->bytes[i] = magic[i];
  }
  return true;
}

void Fuzzer::ReplaceTargetCmdArg(ThreadContext *tc, const char *search, const char *replace) {
  for (int i = 0; i < tc->target_argc; i++) {
    if (strcmp(tc->target_argv[i], search) == 0) {
      char* arg = (char*)malloc(strlen(replace) + 1);
      strcpy(arg, replace);
      tc->target_argv[i] = arg;
    }
  }
}



PRNG *Fuzzer::CreatePRNG(int argc, char **argv, ThreadContext *tc) {
  return new MTPRNG();
}

Instrumentation *Fuzzer::CreateInstrumentation(int argc, char **argv, ThreadContext *tc) {
  TinyInstInstrumentation *instrumentation = new TinyInstInstrumentation();
  instrumentation->Init(argc, argv);
  return instrumentation;
}

SampleDelivery *Fuzzer::CreateSampleDelivery(int argc, char **argv, ThreadContext *tc) {
  char *option = GetOption("-delivery", argc, argv);
  if (!option || !strcmp(option, "file")) {

    string outfile = DirJoin(out_dir, string("input_") + std::to_string(tc->thread_id));
    ReplaceTargetCmdArg(tc, "@@", outfile.c_str());

    FileSampleDelivery* sampleDelivery = new FileSampleDelivery();
    sampleDelivery->Init(argc, argv);
    sampleDelivery->SetFilename(outfile);
    return sampleDelivery;

  } else if (!strcmp(option, "shmem")) {

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
    string shm_name = string("shm_fuzz_") + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(tc->thread_id);
#else
    string shm_name = string("/shm_fuzz_") + std::to_string(getpid()) + "_" + std::to_string(tc->thread_id);
#endif
    ReplaceTargetCmdArg(tc, "@@", shm_name.c_str());

    SHMSampleDelivery* sampleDelivery = new SHMSampleDelivery((char*)shm_name.c_str(), MAX_SAMPLE_SIZE + 4);
    sampleDelivery->Init(argc, argv);
    return sampleDelivery;
  } else {
    FATAL("Unknown sample delivery option");
  }
}

bool Fuzzer::OutputFilter(Sample *original_sample, Sample *output_sample) {
  // use the original sample
  return false;
}
