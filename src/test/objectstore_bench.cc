// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <chrono>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "os/ObjectStore.h"

#include "global/global_init.h"

#include "common/strtol.h"
#include "common/ceph_argparse.h"

#define dout_subsys ceph_subsys_filestore

static void usage()
{
  derr << "usage: osbench [flags]\n"
      "	 --size\n"
      "	       total size in bytes\n"
      "	 --block-size\n"
      "	       block size in bytes for each write\n"
      "	 --repeats\n"
      "	       number of times to repeat the write cycle\n"
      "	 --threads\n"
      "	       number of threads to carry out this workload\n"
      "	 --multi-object\n"
      "	       have each thread write to a separate object\n" << dendl;
  generic_server_usage();
}

// helper class for bytes with units
struct byte_units {
  size_t v;
  byte_units(size_t v) : v(v) {}

  bool parse(const std::string &val);

  operator size_t() const { return v; }
};

bool byte_units::parse(const std::string &val)
{
  char *endptr;
  errno = 0;
  unsigned long long ret = strtoull(val.c_str(), &endptr, 10);
  if (errno == ERANGE && ret == ULLONG_MAX)
    return false;
  if (errno && ret == 0)
    return false;
  if (endptr == val.c_str())
    return false;

  // interpret units
  int lshift = 0;
  switch (*endptr) {
    case 't':
    case 'T':
      lshift += 10;
      // cases fall through
    case 'g':
    case 'G':
      lshift += 10;
    case 'm':
    case 'M':
      lshift += 10;
    case 'k':
    case 'K':
      lshift += 10;
      if (*++endptr)
        return false;
    case 0:
      break;

    default:
      return false;
  }

  // test for overflow
  typedef std::numeric_limits<unsigned long long> limits;
  if (ret & ~((1ull << (limits::digits - lshift))-1))
    return false;

  v = ret << lshift;
  return true;
}

std::ostream& operator<<(std::ostream &out, const byte_units &amount)
{
  static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
  static const int max_units = sizeof(units)/sizeof(*units);

  int unit = 0;
  auto v = amount.v;
  while (v >= 1024 && unit < max_units) {
    // preserve significant bytes
    if (v < 1048576 && (v % 1024 != 0))
      break;
    v >>= 10;
    unit++;
  }
  return out << v << ' ' << units[unit];
}

struct Config {
  byte_units size;
  byte_units block_size;
  int repeats;
  int threads;
  bool multi_object;
  Config()
    : size(1048576), block_size(4096),
      repeats(1), threads(1),
      multi_object(false) {}
};

class C_NotifyCond : public Context {
  std::mutex *mutex;
  std::condition_variable *cond;
  bool *done;
public:
  C_NotifyCond(std::mutex *mutex, std::condition_variable *cond, bool *done)
    : mutex(mutex), cond(cond), done(done) {}
  void finish(int r) {
    std::lock_guard<std::mutex> lock(*mutex);
    *done = true;
    cond->notify_one();
  }
};

void osbench_worker(ObjectStore *os, const Config &cfg,
                    const coll_t cid, const ghobject_t oid,
                    uint64_t starting_offset)
{
  bufferlist data;
  data.append(buffer::create(cfg.block_size));

  dout(0) << "Writing " << cfg.size
      << " in blocks of " << cfg.block_size << dendl;

  assert(starting_offset < cfg.size);
  assert(starting_offset % cfg.block_size == 0);

  ObjectStore::Sequencer sequencer("osbench");

  for (int i = 0; i < cfg.repeats; ++i) {
    uint64_t offset = starting_offset;
    size_t len = cfg.size;

    list<ObjectStore::Transaction*> tls;

    std::cout << "Write cycle " << i << std::endl;
    while (len) {
      size_t count = len < cfg.block_size ? len : (size_t)cfg.block_size;

      auto t = new ObjectStore::Transaction;
      t->write(cid, oid, offset, count, data);
      tls.push_back(t);

      offset += count;
      if (offset > cfg.size)
        offset -= cfg.size;
      len -= count;
    }

    // set up the finisher
    std::mutex mutex;
    std::condition_variable cond;
    bool done = false;

    os->queue_transactions(&sequencer, tls, nullptr,
                           new C_NotifyCond(&mutex, &cond, &done));

    std::unique_lock<std::mutex> lock(mutex);
    cond.wait(lock, [&done](){ return done; });
    lock.unlock();

    while (!tls.empty()) {
      auto t = tls.front();
      tls.pop_front();
      delete t;
    }
  }
}

int main(int argc, const char *argv[])
{
  Config cfg;

  // command-line arguments
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);

  global_init(nullptr, args, CEPH_ENTITY_TYPE_OSD, CODE_ENVIRONMENT_UTILITY, 0);

  std::string val;
  vector<const char*>::iterator i = args.begin();
  while (i != args.end()) {
    if (ceph_argparse_double_dash(args, i))
      break;

    if (ceph_argparse_witharg(args, i, &val, "--size", (char*)nullptr)) {
      if (!cfg.size.parse(val)) {
        derr << "error parsing size: It must be an int." << dendl;
        usage();
      }
    } else if (ceph_argparse_witharg(args, i, &val, "--block-size", (char*)nullptr)) {
      if (!cfg.block_size.parse(val)) {
        derr << "error parsing block-size: It must be an int." << dendl;
        usage();
      }
    } else if (ceph_argparse_witharg(args, i, &val, "--repeats", (char*)nullptr)) {
      cfg.repeats = atoi(val.c_str());
    } else if (ceph_argparse_witharg(args, i, &val, "--threads", (char*)nullptr)) {
      cfg.threads = atoi(val.c_str());
    } else if (ceph_argparse_flag(args, i, "--multi-object", (char*)nullptr)) {
      cfg.multi_object = true;
    } else {
      derr << "Error: can't understand argument: " << *i << "\n" << dendl;
      usage();
    }
  }

  common_init_finish(g_ceph_context);

  // create object store
  dout(0) << "objectstore " << g_conf->osd_objectstore << dendl;
  dout(0) << "data " << g_conf->osd_data << dendl;
  dout(0) << "journal " << g_conf->osd_journal << dendl;
  dout(0) << "size " << cfg.size << dendl;
  dout(0) << "block-size " << cfg.block_size << dendl;
  dout(0) << "repeats " << cfg.repeats << dendl;
  dout(0) << "threads " << cfg.threads << dendl;

  auto os = std::unique_ptr<ObjectStore>(
      ObjectStore::create(g_ceph_context,
                          g_conf->osd_objectstore,
                          g_conf->osd_data,
                          g_conf->osd_journal));
  if (!os) {
    derr << "bad objectstore type " << g_conf->osd_objectstore << dendl;
    return 1;
  }
  if (os->mkfs() < 0) {
    derr << "mkfs failed" << dendl;
    return 1;
  }
  if (os->mount() < 0) {
    derr << "mount failed" << dendl;
    return 1;
  }

  dout(10) << "created objectstore " << os.get() << dendl;

  // create a collection
  spg_t pg;
  const coll_t cid(pg);
  {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    os->apply_transaction(t);
  }

  // create the objects
  std::vector<ghobject_t> oids;
  if (cfg.multi_object) {
    oids.reserve(cfg.threads);
    for (int i = 0; i < cfg.threads; i++) {
      std::stringstream oss;
      oss << "osbench-thread-" << i;
      oids.emplace_back(pg.make_temp_object(oss.str()));

      ObjectStore::Transaction t;
      t.touch(cid, oids[i]);
      int r = os->apply_transaction(t);
      assert(r == 0);
    }
  } else {
    oids.emplace_back(pg.make_temp_object("osbench"));

    ObjectStore::Transaction t;
    t.touch(cid, oids.back());
    int r = os->apply_transaction(t);
    assert(r == 0);
  }

  // run the worker threads
  std::vector<std::thread> workers;
  workers.reserve(cfg.threads);

  using namespace std::chrono;
  auto t1 = high_resolution_clock::now();
  for (int i = 0; i < cfg.threads; i++) {
    const auto &oid = cfg.multi_object ? oids[i] : oids[0];
    workers.emplace_back(osbench_worker, os.get(), std::ref(cfg),
                         cid, oid, i * cfg.size / cfg.threads);
  }
  for (auto &worker : workers)
    worker.join();
  auto t2 = high_resolution_clock::now();
  workers.clear();

  auto duration = duration_cast<microseconds>(t2 - t1);
  byte_units total = cfg.size * cfg.repeats * cfg.threads;
  byte_units rate = (1000000LL * total) / duration.count();
  size_t iops = (1000000LL * total / cfg.block_size) / duration.count();
  dout(0) << "Wrote " << total << " in "
      << duration.count() << "us, at a rate of " << rate << "/s and "
      << iops << " iops" << dendl;

  // remove the objects
  ObjectStore::Transaction t;
  for (const auto &oid : oids)
    t.remove(cid, oid);
  os->apply_transaction(t);

  os->umount();
  return 0;
}
