#define NDEBUG

#include <iostream>
#include <string>
#include <assert.h>
#include "palmtree.h"
#include <thread>
#include <cstdlib>
#include <glog/logging.h>
#include <map>
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_JEMALLOC_H
#include <jemalloc/jemalloc.h>
#endif

#ifdef HAVE_STX_BTREE_MAP_H
#include <stx/btree_map.h>
#endif
#ifdef HAVE_TLX_CONTAINER_BTREE_MAP_HPP
#include <tlx/container/btree_map.hpp>
#endif

#include <pthread.h>
#include "CycleTimer.h"

#undef min
#undef max


using namespace std;

int worker_num;

class fast_random {
 public:
  fast_random(unsigned long seed) : seed(0) { set_seed0(seed); }

  inline unsigned long next() {
    return ((unsigned long)next(32) << 32) + next(32);
  }

  inline uint32_t next_u32() { return next(32); }

  inline uint16_t next_u16() { return (uint16_t)next(16); }

  /** [0.0, 1.0) */
  inline double next_uniform() {
    return (((unsigned long)next(26) << 27) + next(27)) / (double)(1L << 53);
  }

  inline char next_char() { return next(8) % 256; }

  inline char next_readable_char() {
    static const char readables[] =
        "0123456789@ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
    return readables[next(6)];
  }

  inline std::string next_string(size_t len) {
    std::string s(len, 0);
    for (size_t i = 0; i < len; i++) s[i] = next_char();
    return s;
  }

  inline std::string next_readable_string(size_t len) {
    std::string s(len, 0);
    for (size_t i = 0; i < len; i++) s[i] = next_readable_char();
    return s;
  }

  inline unsigned long get_seed() { return seed; }

  inline void set_seed(unsigned long seed) { this->seed = seed; }

 private:
  inline void set_seed0(unsigned long seed) {
    this->seed = (seed ^ 0x5DEECE66DL) & ((1L << 48) - 1);
  }

  inline unsigned long next(unsigned int bits) {
    seed = (seed * 0x5DEECE66DL + 0xBL) & ((1L << 48) - 1);
    return (unsigned long)(seed >> (48 - bits));
  }

  unsigned long seed;
};

void test() {
  palmtree::PalmTree<int, int> palmtree(std::numeric_limits<int>::min(), worker_num);
  palmtree::PalmTree<int, int> *palmtreep = &palmtree;

  for (int i = 0; i < 32; i++) {
    palmtreep->insert(i, i);
  }

  for (int i = 16; i <= 30; i++) {
     palmtreep->remove(i);
  }

  for (int i = 0; i <= 15; i++) {
    palmtreep->remove(i);
  }

  palmtreep->remove(31);

  for (int i = 0; i < 32; i++) {
    DLOG(INFO) << "Remove " << i;
    palmtreep->remove(i);
    int res;
    DLOG(INFO) << "Find " << i;
    bool success = palmtreep->find(i, res);
    if (success) {
      assert(false);
    } else {
      DLOG(INFO) << "Thread " << i << " get nothing";
    }
  }

  srand(15618);

  std::map<int, int> reference;
  for (int i = 10; i < 256; i++) {
    int key1 = i;
    int value1 = rand() % 10;
    int key2 = i - 10;

    palmtreep->insert(key1, value1);
    palmtreep->remove(key2);

    reference.emplace(key1, value1);
    reference.erase(key2);
  }

  for (auto itr = reference.begin(); itr != reference.end(); itr++) {
    DLOG(INFO) << itr->first << " " << itr->second;
  }

  for (int i = 246; i < 256; i++) {
    int res;
    bool suc = palmtreep->find(i, res);
    CHECK(suc == true && res == reference[i]) << "Should find " << i << " " << reference[i];
  }

  while(palmtree.task_nums > 0)
    ;
}

void bench() {
  int *buff = new int[TEST_SIZE];
  for(int i = 0; i < TEST_SIZE; i++) {
    buff[i] = i;
  }

  std::random_shuffle(buff, buff + TEST_SIZE);

  palmtree::PalmTree<int, int> palmtree(std::numeric_limits<int>::min(), worker_num);
  palmtree::PalmTree<int, int> *palmtreep = &palmtree;

  std::vector<std::thread> threads;

  double start = CycleTimer::currentSeconds();

  for (int i = 0; i < 1; i++) {
    threads.push_back(std::thread([palmtreep, i, buff]() {
      for(int j = 0; j < TEST_SIZE; j++) {
        auto kv = buff[j];
        int res;
        palmtreep->insert(kv, kv);
        palmtreep->find(kv, res);
      }
    }));
  }

  for (auto &thread : threads)
    thread.join();

  delete[] buff;
  LOG(INFO) << "task_nums: " << palmtree.task_nums;
  while(palmtree.task_nums > 0)
    ;

  double end = CycleTimer::currentSeconds();
  cout << "run for " << end-start << "s";
}

// Populate a palm tree with @entry_count entries
void populate_palm_tree(palmtree::PalmTree<int, int> *palmtreep, size_t entry_count) {
  LOG(INFO) << "Begin populate palm tree with " << entry_count << " entries";
  int *buff = new int[entry_count];
  for(size_t i = 0; i < entry_count; i++) {
    buff[i] = i;
  }

  std::random_shuffle(buff, buff + entry_count);

  double bt = CycleTimer::currentSeconds();
  for(size_t j = 0; j < entry_count; j++) {
    // auto kv = buff[j];
    palmtreep->insert(2 * j, 2 * j);
  }

  delete[] buff;

  // Wait for task finished
  palmtreep->wait_finish();
  double passed = CycleTimer::currentSeconds() - bt;
  double thput = entry_count / passed / 1024;
  LOG(INFO) << "Populated " << entry_count << " keys in " << passed << " secs, thruput: " << thput;
}

void readonly_skew(size_t entry_count, size_t op_count, float contention_ratio, bool run_std_map = false) {
  LOG(INFO) << "Begin palmtree readonly skew benchmark, contention ratio: " << contention_ratio;
  palmtree::PalmTree<int, int> palmtree(std::numeric_limits<int>::min(), worker_num);
  palmtree::PalmTree<int, int> *palmtreep = &palmtree;

  populate_palm_tree(palmtreep, entry_count);
  // Reset the metrics
  palmtreep->reset_metric();

  // Wait for insertion finished
  LOG(INFO) << entry_count << " entries inserted";

  fast_random rng(time(0));

  double start = CycleTimer::currentSeconds();
  LOG(INFO) << "Benchmark started";

  int one_step = entry_count / (palmtreep->batch_size()+1);
  int last_key = 0;
  int batch_task_count = 0;
  for (size_t i = 0; i < op_count; i++) {
    last_key += rng.next_u32() % one_step;
    last_key %= entry_count;
    batch_task_count++;

    auto id = rng.next_uniform();
    auto k = last_key;
    if(id < contention_ratio) {
      k = (int) (k * 0.2);
    }
    int res;
    palmtreep->find(2 * k, res);
    if (batch_task_count >= palmtreep->batch_size()) {
      batch_task_count = 0;
      last_key = 0;
    }
  }

  LOG(INFO) << palmtreep->task_nums << " left";
  palmtreep->wait_finish();
  double end = CycleTimer::currentSeconds();
  LOG(INFO) << "Palmtree run for " << end-start << "s, " << "thput: " << std::fixed << 2 * op_count/(end-start)/1000 << " K rps";
  double runtime = (end-start) / 2;

  if (run_std_map) {
    LOG(INFO) << "Running std map";
    std::map<int, int> map;
    for (size_t i = 0; i < entry_count; i++)
      map.insert(std::make_pair(i, i));

    pthread_rwlock_t lock_rw = PTHREAD_RWLOCK_INITIALIZER;
    pthread_rwlock_t *l = &lock_rw;

    auto map_p = &map;
    start = CycleTimer::currentSeconds();
    std::vector<std::thread> threads;


    auto w_n = worker_num;
    for(int i = 0; i < w_n; i++) {
      threads.push_back(std::thread([map_p, op_count, entry_count, l, w_n, contention_ratio]() {
        fast_random rng(time(0));
        for (size_t i = 0; i < op_count / w_n; i++) {
          int rand_key = rng.next_u32() % entry_count;
          auto id = rng.next_uniform();
          if(id < contention_ratio) {
            rand_key = (int) (rand_key * 0.2);
          }
          pthread_rwlock_rdlock(l);
          map_p->find(rand_key);
          pthread_rwlock_unlock(l);
        }
      }));
    }

    for(auto &t : threads) {
      t.join();
    }
    end = CycleTimer::currentSeconds();
    LOG(INFO) << "std::map run for " << end-start << "s, " << "thput:" << std::fixed << op_count/(end-start)/1000 << " K rps";
    double runtime_ref = end-start;
    LOG(INFO) << "SPEEDUP over std map: " << runtime_ref / runtime << " X";

    threads.clear();

#ifdef HAVE_STX_BTREE_MAP_H
    // stx
    LOG(INFO) << "Running stx map";
    
    stx::btree_map<int, int> stx_map;
    for (size_t i = 0; i < entry_count; i++)
      stx_map.insert(std::make_pair(i, i));

    start = CycleTimer::currentSeconds();
    auto stx_p = &stx_map;
    for(int i = 0; i < w_n; i++) {
      threads.push_back(std::thread([stx_p, op_count, entry_count, l, w_n, contention_ratio]() {
        fast_random rng(time(0));
        for (size_t i = 0; i < op_count / w_n; i++) {
          int rand_key = rng.next_u32() % entry_count;
          auto id = rng.next_uniform();
          if(id < contention_ratio) {
            rand_key = (int) (rand_key * 0.2);
          }
          pthread_rwlock_rdlock(l);
          stx_p->find(rand_key);
          pthread_rwlock_unlock(l);
        }
      }));
    }

    for(auto &t : threads) {
      t.join();
    }

    end = CycleTimer::currentSeconds();
    LOG(INFO) << "stx map run for " << end-start << "s, " << "thput:" << std::fixed << op_count/(end-start)/1000 << " K rps";

    runtime_ref = end-start;
	  LOG(INFO) << "STX SPEEDUP over PalmTree: " << runtime_ref / runtime << " X";
#endif

#ifdef HAVE_TLX_CONTAINER_BTREE_MAP_HPP
	  // tlx (successor of stx)
	  LOG(INFO) << "Running tlx map";

	  tlx::btree_map<int, int> tlx_map;
	  for (size_t i = 0; i < entry_count; i++)
		  tlx_map.insert(std::make_pair(i, i));

	  start = CycleTimer::currentSeconds();
	  auto tlx_p = &tlx_map;
	  for (int i = 0; i < w_n; i++) {
		  threads.push_back(std::thread([tlx_p, op_count, entry_count, l, w_n, contention_ratio]() {
			  fast_random rng(time(0));
			  for (size_t i = 0; i < op_count / w_n; i++) {
				  int rand_key = rng.next_u32() % entry_count;
				  auto id = rng.next_uniform();
				  if (id < contention_ratio) {
					  rand_key = (int)(rand_key * 0.2);
				  }
				  pthread_rwlock_rdlock(l);
				  tlx_p->find(rand_key);
				  pthread_rwlock_unlock(l);
			  }
			  }));
	  }

	  for (auto& t : threads) {
		  t.join();
	  }

	  end = CycleTimer::currentSeconds();
	  LOG(INFO) << "tlx map run for " << end - start << "s, " << "thput:" << std::fixed << op_count / (end - start) / 1000 << " K rps";

	  runtime_ref = end - start;
	  LOG(INFO) << "TLX SPEEDUP over PalmTree: " << runtime_ref / runtime << " X";
  }
#endif
  }
}





void update_skew(size_t entry_count, size_t op_count, float contention_ratio, bool run_std_map = false) {
  LOG(INFO) << "Begin palmtree update skew benchmark, contention ratio: " << contention_ratio;
  // palmtree::PalmTree<int, int> palmtree(std::numeric_limits<int>::min(), worker_num);
  palmtree::PalmTree<int, int> *palmtreep = new palmtree::PalmTree<int, int> (std::numeric_limits<int>::min(), worker_num);;

  populate_palm_tree(palmtreep, entry_count);
  // Reset the metrics
  palmtreep->reset_metric();

  // Wait for insertion finished
  LOG(INFO) << entry_count << " entries inserted";

  fast_random rng(time(0));

  double start = CycleTimer::currentSeconds();
  LOG(INFO) << "Benchmark started";

  int one_step = 2 * entry_count / (palmtreep->batch_size()+1);
  int last_key = 0;
  int batch_task_count = 0;
  for (size_t i = 0; i < op_count; i++) {
    last_key += rng.next_u32() % one_step;
    last_key %= entry_count;
    batch_task_count++;
    auto id = rng.next_uniform();
    int k = last_key;
    if(id < contention_ratio) {
      k = (int) (k * 0.2);
    }

    id = rng.next_uniform();

    if(id < 0.1) {
      palmtreep->insert(last_key, last_key);
    } else if(id < 0.2) {
      palmtreep->remove(last_key);
    }else {
      int res;
      palmtreep->find(last_key, res);
    }

    if (batch_task_count >= palmtreep->batch_size()) {
      batch_task_count = 0;
      last_key = 0;
    }
  }

  LOG(INFO) << palmtreep->task_nums << " left";
  palmtreep->wait_finish();
  double end = CycleTimer::currentSeconds();
  LOG(INFO) << "Palmtree run for " << end-start << "s, " << "thput: " << std::fixed << 2 * op_count/(end-start)/1000 << " K rps";
  double runtime = (end-start) / 2;

  delete palmtreep;

  if (run_std_map) {
    LOG(INFO) << "Running std map";
    std::map<int, int> map;
    for (size_t i = 0; i < entry_count; i++)
      map.insert(std::make_pair(i, i));


    pthread_rwlock_t lock_rw = PTHREAD_RWLOCK_INITIALIZER;
    pthread_rwlock_t *l = &lock_rw;


    start = CycleTimer::currentSeconds();
    auto map_p = &map;
    start = CycleTimer::currentSeconds();
    std::vector<std::thread> threads;

    auto w_n = worker_num;
    for(int i = 0; i < w_n; i++) {
      threads.push_back(std::thread([map_p, op_count, entry_count, l, w_n, contention_ratio]() {
        fast_random rng(time(0));

        auto map = *map_p;
        for (size_t i = 0; i < op_count / w_n; i++) {
          int k = rng.next_u32() % entry_count;
          auto id = rng.next_uniform();

          auto rand_key = k;
          if(id < contention_ratio) {
            rand_key = (int) rand_key * 0.2;
          }
          id = rng.next_uniform();
          if(id < 0.1) {
            pthread_rwlock_wrlock(l);
            map[rand_key] = rand_key;
          }else if (id < 0.2) {
            pthread_rwlock_wrlock(l);
            map.erase(rand_key);
          }else {
            pthread_rwlock_rdlock(l);
            map.find(rand_key);
          }
          pthread_rwlock_unlock(l);
        }
      }));
    }

    for(auto &t : threads) {
      t.join();
    }

    threads.clear();

    end = CycleTimer::currentSeconds();
    LOG(INFO) << "std::map run for " << end-start << "s, " << "thput:" << std::fixed << op_count/(end-start)/1000 << " K rps";

    double runtime_ref = end-start;
    LOG(INFO) << "SPEEDUP over PalmTree: " << runtime_ref / runtime << " X";

#ifdef HAVE_STX_BTREE_MAP_H
    // stx
    LOG(INFO) << "Running stx map";
    stx::btree_map<int, int> stx_map;
    for (size_t i = 0; i < entry_count; i++)
      stx_map.insert(std::make_pair(i, i));

    start = CycleTimer::currentSeconds();
    auto stx_p = &stx_map;
    for(int i = 0; i < w_n; i++) {
      threads.push_back(std::thread([stx_p, op_count, entry_count, l, w_n, contention_ratio]() {
        fast_random rng(time(0));
        auto stx = *stx_p;
        for (size_t i = 0; i < op_count / w_n; i++) {
          int k = rng.next_u32() % entry_count;
          auto id = rng.next_uniform();

          auto rand_key = k;
          if(id < contention_ratio) {
            rand_key = (int) rand_key * 0.2;
          }

          id = rng.next_uniform();
          if(id < 0.1) {
            pthread_rwlock_wrlock(l);
            stx.insert(rand_key, rand_key);
          }else if (id < 0.2) {
            pthread_rwlock_wrlock(l);
            stx.erase(rand_key);
          }else {
            pthread_rwlock_rdlock(l);
            stx.find(rand_key);
          }

          pthread_rwlock_unlock(l);
        }
      }));
    }

    for(auto &t : threads) {
      t.join();
    }

    end = CycleTimer::currentSeconds();
    LOG(INFO) << "stx map run for " << end-start << "s, " << "thput:" << std::fixed << op_count/(end-start)/1000 << " K rps";

    runtime_ref = end-start;
    LOG(INFO) << "STX SPEEDUP over PalmTree: " << runtime_ref / runtime << " X";
#endif

#ifdef HAVE_TLX_CONTAINER_BTREE_MAP_HPP
	// tlx (successor of stx)
	LOG(INFO) << "Running tlx map";
	tlx::btree_map<int, int> tlx_map;
	for (size_t i = 0; i < entry_count; i++)
		tlx_map.insert(std::make_pair(i, i));

	start = CycleTimer::currentSeconds();
	auto tlx_p = &tlx_map;
	for (int i = 0; i < w_n; i++) {
		threads.push_back(std::thread([tlx_p, op_count, entry_count, l, w_n, contention_ratio]() {
			fast_random rng(time(0));
			auto stx = *tlx_p;
			for (size_t i = 0; i < op_count / w_n; i++) {
				int k = rng.next_u32() % entry_count;
				auto id = rng.next_uniform();

				auto rand_key = k;
				if (id < contention_ratio) {
					rand_key = (int)rand_key * 0.2;
				}

				id = rng.next_uniform();
				if (id < 0.1) {
					pthread_rwlock_wrlock(l);
					stx.insert(rand_key, rand_key);
				}
				else if (id < 0.2) {
					pthread_rwlock_wrlock(l);
					stx.erase(rand_key);
				}
				else {
					pthread_rwlock_rdlock(l);
					stx.find(rand_key);
				}

				pthread_rwlock_unlock(l);
			}
			}));
	}

	for (auto& t : threads) {
		t.join();
	}

	end = CycleTimer::currentSeconds();
	LOG(INFO) << "tlx map run for " << end - start << "s, " << "thput:" << std::fixed << op_count / (end - start) / 1000 << " K rps";

	runtime_ref = end - start;
	LOG(INFO) << "TLX SPEEDUP over PalmTree: " << runtime_ref / runtime << " X";
#endif
  }
}

void run_insert_bench(size_t entry_count) {
  LOG(INFO) << "Begin palmtree insert benchmark, inserting " << entry_count << " entries";

  palmtree::PalmTree<int, int> *palmtreep = new palmtree::PalmTree<int, int> (std::numeric_limits<int>::min(), worker_num);

  fast_random rng(time(0));
  int batch_size = palmtreep->batch_size();
  int step = 5000;

  int num_batch = entry_count / batch_size;
  int residue = entry_count % batch_size;

  unsigned start = 0;

  auto bt = CycleTimer::currentSeconds();
  for (int i = 0; i < num_batch; i++) {
    start = 0;
    for (int j = 0; j < batch_size; j++) {
      start += rng.next_u32() % step;
      palmtreep->insert(start, start);  
    }
  }

  start = 0;
  for (int i = 0; i < residue; i++) {
    start += rng.next_u32() % 32; 
    palmtreep->insert(start, start);
  }

  palmtreep->wait_finish();
  auto passed = CycleTimer::currentSeconds() - bt;
  LOG(INFO) << "Finished after " << passed << " ms, thput: " << entry_count / passed / 1000 << " K/s";

  delete palmtreep;


  LOG(INFO) << "Begin STX BTree insert benchmark, inserting " << entry_count << " entries";
  stx::btree_map<int, int> stx_map;
  bt = CycleTimer::currentSeconds();
  auto stx_p = &stx_map;
  int w_n = worker_num;

  std::vector<std::thread> threads;
  for(int i = 0; i < w_n; i++) {
    threads.push_back(std::thread([stx_p, entry_count, w_n]() {
      fast_random rng(time(0));
      for (size_t i = 0; i < entry_count / w_n; i++) {
        int rand_key = rng.next_u32() % (entry_count * 32);
        lock.lock();
        stx_p->insert(rand_key, rand_key);
        lock.unlock();
      }
    }));
  }

  for (auto &thread : threads) {
    thread.join();
  }
  passed = CycleTimer::currentSeconds() - bt;
  LOG(INFO) << "Finished after " << passed << " ms, thput: " << entry_count / passed / 1000 << " K/s";


}


int main(int argc, char *argv[]) {
  // Google logging
  FLAGS_logtostderr = 1;
  google::InitGoogleLogging(argv[0]);

  if(argc < 5) {
    // print usage
    cout << "usage example: 8 true r 0.8" << endl;
    cout << "\trunning 8 workers, running map to compare performance, readonly, contention ratio 0.8" << endl;
    exit(0);
  }

  worker_num = atoi(argv[1]);
  bool c;
  if(strcmp(argv[2], "true") == 0) {
    c = true;
  }else{
    c = false;
  }

  bool r = false;
  bool i = false;
  if(strcmp(argv[3], "r") == 0) {
    r = true;
  }else{
    r = false;
  }

  if (strcmp(argv[3], "i") == 0) {
    i = true;
  } else {
    i = false;
  }

  if (i) {
    auto insert = 1024 * 512 * 10;
    run_insert_bench(insert);
    return 0;
  }

  float contention_ratio;

  contention_ratio = atof(argv[4]);

  auto insert = 1024 * 512 * 10;
  auto op_num = 1024 * 1024 * 10;
  // readonly_uniform(insert, op_num, c);
  if(r) {
    readonly_skew(insert, op_num, contention_ratio, c);
  }else {
    update_skew(insert, op_num, contention_ratio, c);
  }

  return 0;
}

