#ifndef HDCS_CORE_STAT_CONTROLLER_H
#define HDCS_CORE_STAT_CONTROLLER_H

#include "Network/hdcs_networking.h"
#include "common/Timer.h"
#include "ha/HDCSCoreStat.h"
#include "ha/HDCSDomainMap.h"

namespace hdcs {

namespace ha {
 
typedef std::map<void*, std::shared_ptr<HDCSCoreStat>> hdcs_core_stat_map_t;

typedef uint8_t HDCS_HA_MSG_TYPE;
#define HDCS_MSG_CORE_STAT  0X32
#define HDCS_HA_CORE_STAT_UPDATE  0XD2

typedef uint8_t HDCS_NODE_STAT_TYPE;
#define HDCS_HA_NODE_STAT_UP  0XDA
#define HDCS_HA_NODE_STAT_DOWN  0XDB
#define HDCS_HA_NODE_STAT_PREP  0XDC

struct HDCS_NODE_STAT_T {
  HDCS_NODE_STAT_TYPE stat;
  hdcs_core_stat_map_t hdcs_core_stat_map;
  HDCS_NODE_STAT_T(): stat(HDCS_HA_NODE_STAT_UP) {
  }
};

struct HDCS_CORE_STAT_MSG_HEADER_T {
  HDCS_HA_MSG_TYPE reserved_flag;
  char node_name[32];
  HDCS_NODE_STAT_TYPE node_stat;
};

struct HDCS_CORE_STAT_MSG_T {
  HDCS_CORE_STAT_MSG_HEADER_T header;
  HDCS_CORE_STAT_T* data;
};

class HDCS_CORE_STAT_MSG {
public:
  HDCS_CORE_STAT_MSG (uint8_t cores_num, HDCS_NODE_STAT_TYPE node_stat, std::string name)
    : cores_num(cores_num),
      data_size(sizeof(HDCS_CORE_STAT_T) * cores_num) {
    data_ = (char*)malloc (sizeof(HDCS_CORE_STAT_MSG_HEADER_T) + data_size);
    memset(data_, 0, sizeof(HDCS_CORE_STAT_MSG_HEADER_T) + data_size);
    ((HDCS_CORE_STAT_MSG_T*)data_)->header.reserved_flag = HDCS_MSG_CORE_STAT; 
    ((HDCS_CORE_STAT_MSG_T*)data_)->header.node_stat = node_stat; 
    memcpy(((HDCS_CORE_STAT_MSG_T*)data_)->header.node_name, name.c_str(), name.size()); 
  }

  HDCS_CORE_STAT_MSG (std::string msg) {
    data_size = msg.length() - sizeof(HDCS_CORE_STAT_MSG_HEADER_T);
    data_ = (char*)malloc (sizeof(HDCS_CORE_STAT_MSG_HEADER_T) + data_size);
    memcpy(data_, msg.c_str(), sizeof(HDCS_CORE_STAT_MSG_HEADER_T) + data_size);
    cores_num = data_size / sizeof(HDCS_CORE_STAT_T);
    printf("received cores_num: %u\n", cores_num);
  }

  ~HDCS_CORE_STAT_MSG() {
    free(data_);
  }

  uint8_t get_cores_num() {
    return cores_num;
  }

  char* get_node_name() {
    return ((HDCS_CORE_STAT_MSG_T*)data_)->header.node_name;
  }
  
  HDCS_NODE_STAT_TYPE get_node_stat() {
    return ((HDCS_CORE_STAT_MSG_T*)data_)->header.node_stat;
  }

  char* data() {
    return data_;
  }

  uint64_t size() {
    return sizeof(HDCS_CORE_STAT_MSG_HEADER_T) + data_size;
  }

  void loadline(uint8_t index, HDCS_CORE_STAT_T *stat) {
    char* stat_data = data_ + sizeof(HDCS_CORE_STAT_MSG_HEADER_T);
    memcpy(stat_data + index, stat, sizeof(HDCS_CORE_STAT_T));
  } 

  void readline(uint8_t index, HDCS_CORE_STAT_T *stat) {
    char* stat_data = data_ + sizeof(HDCS_CORE_STAT_MSG_HEADER_T);
    memcpy(stat, stat_data + index, sizeof(HDCS_CORE_STAT_T));
  } 

private:
  char* data_;
  uint8_t cores_num;
  uint64_t data_size;
};

class HDCSCoreStatController {
public:
  HDCSCoreStatController(std::string node_name,
                         HDCSDomainMap *domain_map = nullptr):
                         conn(nullptr),
                         domain_map(domain_map),
                         node_name(node_name) {
  }

  ~HDCSCoreStatController() {
    if (conn)
      conn->close();
  }

  void set_conn (networking::Connection* new_conn) {
    conn = new_conn;
  }

  void set_host_list (std::vector<std::string> host_list) {
    for (auto &host : host_list) {
      ha_hdcs_node_stat[host] = new HDCS_NODE_STAT_T();
    }
  }

  std::shared_ptr<HDCSCoreStat> register_core (void* hdcs_core_id) {
    std::shared_ptr<AioCompletion> error_handler = std::make_shared<AioCompletionImp>([&](ssize_t r){
      send_stat_map();
      printf("one core stat is updated, send out new stat map\n");
    }, -1);
    auto it = hdcs_core_stat_map.insert(
      std::pair<void*, std::shared_ptr<HDCSCoreStat>>(
        hdcs_core_id,
        std::make_shared<HDCSCoreStat>(hdcs_core_id, error_handler)
      )
    );
    return it.first->second;
  }

  int unregister_core (void* hdcs_core_id) {
    hdcs_core_stat_map.erase(hdcs_core_id);
    return 0;
  }

  hdcs_core_stat_map_t* get_stat_map() {
    return &hdcs_core_stat_map;
  }

  void send_stat_map() {
    HDCS_CORE_STAT_MSG msg_content(hdcs_core_stat_map.size(), node_stat, node_name);
    uint8_t i = 0;
    for (auto &item : hdcs_core_stat_map) {
      msg_content.loadline(i++, item.second->get_stat());
    }
    printf ("hdcs_core_stat_controller conn: %p\n", conn);
    conn->aio_communicate(std::move(std::string(msg_content.data(), msg_content.size())));
  }

  void request_handler(void* session_id, std::string msg_content) {
    printf("received new hdcs core stat update\n");
    HDCS_CORE_STAT_MSG core_stat_msg(msg_content);
    HDCS_CORE_STAT_T tmp_stat;
    for (uint8_t i = 0; i < core_stat_msg.get_cores_num(); i++) {
      core_stat_msg.readline(i, &tmp_stat);
      ha_hdcs_core_stat[tmp_stat.hdcs_core_id] = tmp_stat.stat;
    }

    auto it = ha_hdcs_node_stat.find(core_stat_msg.get_node_name());
    assert (it != ha_hdcs_node_stat.end());
    it->second->stat = core_stat_msg.get_node_stat();
    HDCS_DOMAIN_ITEM_TYPE domain_item = domain_map->get_host_domain_item(core_stat_msg.get_node_name());
    std::cout << "Get domain items: ";
    domain_map->print(domain_item);
    std::cout << std::endl;

    //print to console
    std::cout << "hostname: " << core_stat_msg.get_node_name() <<
      " status: ";
    printf("%x\n", core_stat_msg.get_node_stat());
    for (auto &print_stat : ha_hdcs_core_stat) {
      printf("new update    ");
      printf("%x", print_stat.first);
      printf(": %x\n", print_stat.second);
    }
  }

  void receiver_handler(void* session_id, std::string msg_content) {

  }

private:
  hdcs_core_stat_map_t hdcs_core_stat_map;
  networking::Connection* conn;
  networking::server* core_stat_listener;
  // as client
  std::map<void*, HDCS_CORE_STAT_TYPE> ha_hdcs_core_stat; 
  HDCS_NODE_STAT_TYPE node_stat;
  std::string node_name;

  // as server
  std::map<std::string, HDCS_NODE_STAT_T*> ha_hdcs_node_stat;
  HDCSDomainMap* domain_map;
};
}// ha
}// hdcs
#endif
