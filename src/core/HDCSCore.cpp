// Copyright [2017] <Intel>

#include "core/HDCSCore.h"
#include "core/policy/CachePolicy.h"
#include "core/policy/TierPolicy.h"
#include "store/SimpleStore/SimpleBlockStore.h"
#include "store/KVStore/KVStore.h"
#include "store/DataStore.h"
#include "store/RBD/RBDImageStore.h"
#include "common/HDCS_REQUEST_HANDLER.h"

#include <string>
#include <boost/algorithm/string.hpp>

namespace hdcs {
namespace core {
HDCSCore::HDCSCore(std::string host, std::string name,
  std::string cfg_file,
  hdcs_repl_options &&replication_options_param):
  host(host),
  name(name),
  replication_options(replication_options_param) {
  config = new Config(host + "_" + name, cfg_file);

  std::string log_path = config->get_config("HDCSCore")["log_to_file"];
  std::cout << "log_path: " << log_path << std::endl;
  if( log_path!="false" ){
    log_path = "hdcs_" + name + ".log";
    int stderr_no = dup(fileno(stderr));
    log_fd = fopen( log_path.c_str(), "w" );
	  if(log_fd==NULL){}
    if(-1==dup2(fileno(log_fd), STDERR_FILENO)){}
  }

  int hdcs_thread_max = stoi(config->get_config("HDCSCore")["op_threads_num"]);
  hdcs_op_threads = new TWorkQueue(hdcs_thread_max);
  uint64_t total_size = stoull(config->get_config("HDCSCore")["total_size"]);
  uint64_t block_size = stoull(config->get_config("HDCSCore")["cache_min_alloc_size"]);
  bool cache_policy_mode = config->get_config("HDCSCore")["policy_mode"].compare(std::string("cache")) == 0 ? true : false;

  std::string engine_type = config->get_config("HDCSCore")["entine_type"];
  std::string path = config->get_config("HDCSCore")["cache_dir_run"];
  std::string pool_name = config->get_config("HDCSCore")["rbd_pool_name"];
  std::string volume_name = name;

  //connect to its replication_nodes
  std::cout << "replication_options.role : " << replication_options.role << std::endl; 
  std::cout << "replication_options.nodes : ";
  for (auto &it :  replication_options.replication_nodes) {
    std::cout << it << ",";
  }
  std::cout << std::endl; 
  if ("master" == replication_options.role && replication_options.replication_nodes.size() != 0) {
    connect_to_replica(replication_options.replication_nodes);
  }

  block_guard = new BlockGuard(total_size, block_size,
                               replication_core_map.size(),
                               std::move(replication_core_map));
  BlockMap* block_ptr_map = block_guard->get_block_map();
  store::DataStore* datastore = nullptr;

  if (cache_policy_mode) {
    uint64_t cache_size = stoull(config->get_config("HDCSCore")["cache_total_size"]);
    float cache_ratio_health = stof(config->get_config("HDCSCore")["cache_ratio_health"]);
    uint64_t timeout_nanosecond = stoull(config->get_config("HDCSCore")["cache_dirty_timeout_nanoseconds"]);
    CACHE_MODE_TYPE cache_mode = config->get_config("HDCSCore")["cache_mode"].compare(std::string("readonly")) == 0 ? CACHE_MODE_READ_ONLY : CACHE_MODE_WRITE_BACK;
    policy = new CachePolicy(total_size, cache_size, block_size, block_ptr_map,
                      datastore->create_engine(engine_type, path, total_size, cache_size, block_size),
                      new store::RBDImageStore(pool_name, volume_name, block_size),
                      cache_ratio_health, &request_queue,
                      timeout_nanosecond, cache_mode, hdcs_thread_max);
  } else {
    policy = new TierPolicy(total_size, block_size, block_ptr_map,
                    datastore->create_engine(engine_type, path, total_size, total_size, block_size),
                    new store::RBDImageStore(pool_name, volume_name, block_size),
                    &request_queue, hdcs_thread_max);
  }

  go = true;
  main_thread = new std::thread(std::bind(&HDCSCore::process, this));

}

HDCSCore::~HDCSCore() {
  go = false;
  main_thread->join();
  delete hdcs_op_threads;
  for (auto& hdcs_replica : replication_core_map) {
    ((hdcs_ioctx_t*)hdcs_replica.second)->conn->close();
    free((hdcs_ioctx_t*)hdcs_replica.second);
  }
  delete policy;
  delete block_guard;
  delete main_thread;
}

void HDCSCore::close() {
#if defined(CACHE_POLICY)
  policy->flush_all();
#endif
}

void HDCSCore::promote_all() {
#if defined(TIER_POLICY)
  ((TierPolicy*)policy)->promote_all();
#endif
}

void HDCSCore::flush_all() {
#if defined(TIER_POLICY)
  ((TierPolicy*)policy)->flush_all();
#endif
}

void HDCSCore::queue_io(std::shared_ptr<Request> req) {
  //request_queue.enqueue((void*)req);
  process_request(req);
}

void HDCSCore::process() {
  while(go){
    std::shared_ptr<Request> req = request_queue.dequeue();
    if (req != nullptr) {
      process_request(req);
    }
  }
}

void HDCSCore::process_request(std::shared_ptr<Request> req) {
  //std::mutex block_request_list_lock;
  //BlockRequestList block_request_list;
  std::lock_guard<std::mutex> lock(block_request_list_lock);
  block_guard->create_block_requests(req, &block_request_list);

  for (BlockRequestList::iterator it = block_request_list.begin(); it != block_request_list.end();) { 
    if (!it->block->in_discard_process) {
      map_block(std::move(*it));
      block_request_list.erase(it++);
    } else {
      it++;
    }
  }
}

void HDCSCore::map_block(BlockRequest &&block_request) {
  BlockOp *block_ops_end;
  Block* block = block_request.block;
  bool do_process = false;
  block->block_mutex.lock();
  // If this block_request belongs to discard block,
  // append to wait list firstly.
  BlockOp *block_ops_head = policy->map(std::move(block_request), &block_ops_end);
  if (!block->in_process) {
    block->in_process = true;
    block->block_ops_end = block_ops_end;
    do_process = true;
    log_print("Block not in process, block: %lu, new end: %p", block->block_id, block_ops_end);
  } else {
    log_print("Block in process, append request, block: %lu, append BlockOp: %p after BlockOp: %p, new end: %p", block->block_id, block_ops_head, block->block_ops_end, block_ops_end);
    block->block_ops_end->block_op_next = block_ops_head;
    block->block_ops_end = block_ops_end;
  }
  block->block_mutex.unlock();
  if (do_process) {
    hdcs_op_threads->add_task(std::bind(&BlockOp::send, block_ops_head, nullptr));
    //block_ops_head->send();
  }
}

void HDCSCore::aio_read (char* data, uint64_t offset, uint64_t length,  void* arg) {
  std::shared_ptr<Request> req = std::make_shared<Request>(IO_TYPE_READ, data, offset, length, arg);
  request_queue.enqueue(req);
  //process_request(req);
}

void HDCSCore::aio_write (char* data, uint64_t offset, uint64_t length,  void* arg) {
  std::shared_ptr<Request> req = std::make_shared<Request>(IO_TYPE_WRITE, data, offset, length, arg);
  //TODO: add TS;
  request_queue.enqueue(req);
  //process_request(req);
}

void HDCSCore::connect_to_replica (std::vector<std::string> replication_nodes) {
  std::string addr;
  std::string port;
  int colon_pos, last_pos;
  char c;

  hdcs_ioctx_t* io_ctx;
  for (auto &addr_port_str : replication_nodes) {
    std::vector<std::string> ip_port;
    boost::split(ip_port, addr_port_str, boost::is_any_of(":"));
    addr = ip_port[0];
    port = ip_port[1];

    io_ctx = (hdcs_ioctx_t*)malloc(sizeof(hdcs_ioctx_t));
    replication_core_map[addr_port_str] = (void*)io_ctx;

    hdcs::networking::ClientOptions client_options;
    client_options._session_num = 5;
    client_options._io_service_num = 5; 
    client_options._thd_num_on_one_session = 3;
    client_options._process_msg = ([](void* p, std::string s){request_handler(p, s);});
    client_options._process_msg_arg = (void*)io_ctx;
    
    io_ctx->conn = new hdcs::networking::Connection(client_options);
    io_ctx->conn->connect(addr, port, (hdcs::networking::TCP_COMMUNICATION)); // TCP
    //io_ctx->conn->connect(addr, port, (hdcs::networking::RDMA_COMMUNICATION)); //RDMA

    hdcs::HDCS_REQUEST_CTX msg_content(HDCS_CONNECT, nullptr, nullptr, 0, name.length(), const_cast<char*>(name.c_str()));
    io_ctx->conn->communicate(std::move(std::string(msg_content.data(), msg_content.size())));
  }
}

}// core
}// hdcs
