#include <iostream>
// #include <kvServer.h>
#include <unistd.h>
#include <iostream>
#include <random>

#include "raft.h"
#include "kvServer.h"

// 用于显示参数帮助信息的函数
void ShowArgsHelp() { std::cout << "format: command -n <nodeNum> -f <configFileName>" << std::endl; }

int main(int argc, char **argv) {
  // 主函数入口，接受命令行参数，如果参数小于2，显示帮助信息并退出
  if (argc < 2) {
    ShowArgsHelp();
    exit(EXIT_FAILURE);
  }

  // 使用 getopt 解析命令行参数，-n 指定节点数量，-f 指定配置文件名
  int c = 0;
  int nodeNum = 0;
  std::string configFileName;
  while ((c = getopt(argc, argv, "n:f:")) != -1) {
    switch (c) {
      case 'n':
        nodeNum = atoi(optarg);
        break;
      case 'f':
        configFileName = optarg;
        break;
      default:
        ShowArgsHelp();
        exit(EXIT_FAILURE);
    }
  }

  // 以追加模式打开配置文件并关闭，确保文件存在，再以截断模式打开，如果打开失败则退出
  std::ofstream file(configFileName, std::ios::out | std::ios::app);
  file.close();
  file = std::ofstream(configFileName, std::ios::out | std::ios::trunc);
  if (file.is_open()) {
    file.close();
    std::cout << configFileName << " 已清空" << std::endl;
  } else {
    std::cout << "无法打开 " << configFileName << std::endl;
    exit(EXIT_FAILURE);
  }

  // 随机数种子生成随机端口号
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(10000, 29999);
  unsigned short startPort = dis(gen);

  for (int i = 0; i < nodeNum; i++) {
    short port = startPort + static_cast<short>(i);
    std::cout << "准备启动 raftkv node:" << i << " port:" << port << " pid:" << getpid() << std::endl;
    pid_t pid = fork();
    if (pid == 0) {
        auto kvServer = new KvServer(i, 500, configFileName, port);
        pause();
    } else if (pid > 0) {
        sleep(3);
    } else {
        std::cerr << "Failed to create child process." << std::endl;
        exit(EXIT_FAILURE);
    }
  }
  pause();
  return 0;
}


