#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <array>
#include <memory>
#include <unistd.h>

// ====== 配置项 ======
const std::string DISK_ID = "/dev/disk/by-id/nvme-SAMSUNG_MZVL2512HCJQ-00BL2_S64JNX0T884912";
const std::string IMG = "/var/lib/libvirt/images/fake-nvme.img";
const std::string MAPPER_NAME = "win11-spliced";
const std::string CONFIG_FILE = "/etc/diskgpt_last_mount.conf";
const std::string VM_CONFIG_FILE = "/etc/diskgpt_last_vm.conf";

struct PartitionInfo {
    int part_num;
    std::string path;
    long long start;
    long long size;
};

std::string exec_and_get(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

long long get_sector_info(const std::string& dev_path, const std::string& type) {
    std::string cmd = "cat /sys/class/block/$(basename $(realpath " + dev_path + "))/" + type + " 2>/dev/null";
    std::string res = exec_and_get(cmd);
    try { return std::stoll(res); } 
    catch (...) { return 0; }
}

void stop_mount() {
    std::cout << "\n[INFO] 正在停止并清理挂载..." << std::endl;
    system(("dmsetup remove " + MAPPER_NAME + " 2>/dev/null").c_str());
    system("for l in $(losetup -a | grep \"fake-nvme.img\" | cut -d: -f1); do losetup -d $l 2>/dev/null; done");
    std::cout << "[OK] 清理完成！" << std::endl;
}

// ================= KVM 与 显卡唤醒逻辑 =================
void prompt_start_vm() {
    std::cout << "\n======================================" << std::endl;
    std::cout << "是否要现在启动 KVM 虚拟机？(Y/n): ";
    std::string choice;
    std::getline(std::cin, choice);
    if (choice != "" && choice != "y" && choice != "Y") {
        std::cout << "[INFO] 已跳过启动虚拟机。" << std::endl;
        return;
    }

    std::cout << "\n--------------------------------------" << std::endl;
    std::cout << "唤醒显卡将使用sudo权限运行：\n";
    std::cout << "  => sudo sh -c \"echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove\"\n";
    std::cout << "  => sudo sh -c \"echo 1 > /sys/bus/pci/rescan\"\n";
    std::cout << "是否需要尝试唤醒独立显卡(Y/n): ";
    std::string gpu_choice;
    std::getline(std::cin, gpu_choice);
    
    if (gpu_choice == "" || gpu_choice == "y" || gpu_choice == "Y") {
        std::cout << "[INFO] 正在执行显卡唤醒指令：" << std::endl;
        system("sh -c \"echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove\" 2>/dev/null");
        sleep(1); 
        system("sh -c \"echo 1 > /sys/bus/pci/rescan\" 2>/dev/null");
        std::cout << "[OK] 显卡唤醒指令已下发！" << std::endl;
    } else {
        std::cout << "[INFO] 已跳过显卡唤醒操作。" << std::endl;
    }
    std::cout << "--------------------------------------\n" << std::endl;

    std::string last_vm;
    if (std::filesystem::exists(VM_CONFIG_FILE)) {
        std::ifstream file(VM_CONFIG_FILE);
        std::getline(file, last_vm);
        file.close();
    }

    if (!last_vm.empty()) {
        std::cout << "[INFO] 发现上次启动的虚拟机: [" << last_vm << "]" << std::endl;
        std::cout << "是否直接启动该虚拟机？ (Y/n): ";
        std::string vm_choice;
        std::getline(std::cin, vm_choice);
        if (vm_choice == "" || vm_choice == "y" || vm_choice == "Y") {
            std::cout << "[INFO] 正在启动虚拟机 " << last_vm << "..." << std::endl;
            system(("virsh start " + last_vm).c_str());
            return;
        }
    }

    std::string vm_list_str = exec_and_get("virsh list --all --name");
    std::vector<std::string> vms;
    std::stringstream ss(vm_list_str);
    std::string vm_name;
    while (std::getline(ss, vm_name)) {
        if (!vm_name.empty()) {
            vms.push_back(vm_name);
        }
    }

    if (vms.empty()) {
        std::cout << "[WARNING] 未扫描到任何 KVM 虚拟机，请检查 libvirt 服务。" << std::endl;
        return;
    }

    std::cout << "扫描到以下 KVM 虚拟机：" << std::endl;
    for (size_t i = 0; i < vms.size(); ++i) {
        std::cout << "  [" << i + 1 << "] " << vms[i] << std::endl;
    }

    std::cout << "请输入要启动的虚拟机序号 (输入 0 取消): ";
    std::string input_idx;
    std::getline(std::cin, input_idx);

    try {
        int idx = std::stoi(input_idx);
        if (idx > 0 && idx <= (int)vms.size()) {
            std::string selected_vm = vms[idx - 1];
            std::ofstream file(VM_CONFIG_FILE);
            file << selected_vm;
            file.close();

            std::cout << "[INFO] 正在启动虚拟机 " << selected_vm << "..." << std::endl;
            system(("virsh start " + selected_vm).c_str());
        } else {
            std::cout << "[INFO] 已取消启动。" << std::endl;
        }
    } catch (...) {
        std::cout << "[INFO] 输入无效，已取消启动。" << std::endl;
    }
}

void start_mount() {
    std::cout << "\n======================================" << std::endl;
    std::cout << "        DiskGPT 磁盘挂载工具" << std::endl;
    std::cout << "======================================\n" << std::endl;

    if (!std::filesystem::exists(DISK_ID)) {
        std::cerr << "[ERROR] 找不到基础硬盘 ID，请检查硬盘是否连接。" << std::endl;
        exit(1);
    }

    std::string input_parts;
    bool use_history = false;

    if (std::filesystem::exists(CONFIG_FILE)) {
        std::ifstream file(CONFIG_FILE);
        std::getline(file, input_parts);
        file.close();
        
        if (!input_parts.empty()) {
            std::cout << "[INFO] 发现上次挂载记录，选择了分区: [" << input_parts << "]" << std::endl;
            std::cout << "是否直接使用上次的配置挂载？ (Y/n): ";
            std::string choice;
            std::getline(std::cin, choice);
            if (choice == "" || choice == "y" || choice == "Y") {
                use_history = true;
            }
        }
    }

    if (!use_history) {
        std::cout << "\n扫描到的可用分区如下：" << std::endl;
        std::string ls_cmd = "ls -1 " + DISK_ID + "-part* 2>/dev/null";
        std::string parts_list = exec_and_get(ls_cmd);
        
        if (parts_list.empty()) {
            std::cerr << "[ERROR] 该硬盘上没有找到任何分区。" << std::endl;
            exit(1);
        }

        std::stringstream ss(parts_list);
        std::string part_path;
        while (std::getline(ss, part_path)) {
            long long size_sectors = get_sector_info(part_path, "size");
            double size_gb = (size_sectors * 512.0) / (1024 * 1024 * 1024);
            
            size_t pos = part_path.find("-part");
            if (pos != std::string::npos) {
                std::string num_str = part_path.substr(pos + 5);
                printf("  [%s] %s (%.2f GB)\n", num_str.c_str(), part_path.c_str(), size_gb);
            }
        }

        std::cout << "\n请输入要挂载的分区号 (用空格分隔，如 '3 4'): ";
        std::getline(std::cin, input_parts);

        std::ofstream file(CONFIG_FILE);
        file << input_parts;
        file.close();
        std::cout << "[INFO] 分区选择已保存，下次可直接加载。" << std::endl;
    }

    std::replace(input_parts.begin(), input_parts.end(), ',', ' ');
    std::stringstream ss_input(input_parts);
    std::vector<PartitionInfo> selected_parts;
    int p_num;
    while (ss_input >> p_num) {
        PartitionInfo pi;
        pi.part_num = p_num;
        pi.path = DISK_ID + "-part" + std::to_string(p_num);
        pi.start = get_sector_info(pi.path, "start");
        pi.size = get_sector_info(pi.path, "size");
        
        if (pi.size == 0) {
            std::cerr << "[WARNING] 无法读取分区 " << p_num << " ，将跳过。" << std::endl;
            continue;
        }
        selected_parts.push_back(pi);
    }

    if (selected_parts.empty()) {
        std::cerr << "[ERROR] 没有有效的分区被选择，程序退出。" << std::endl;
        exit(1);
    }

    std::sort(selected_parts.begin(), selected_parts.end(), [](const PartitionInfo& a, const PartitionInfo& b) {
        return a.start < b.start;
    });

    std::cout << "\n[INFO] 正在清理旧环境..." << std::endl;
    stop_mount();

    std::cout << "[INFO] 准备基础镜像与映射表..." << std::endl;
    long long total_sectors = get_sector_info(DISK_ID, "size");
    system("mkdir -p /var/lib/libvirt/images/");
    system(("truncate -s " + std::to_string(total_sectors * 512) + " " + IMG).c_str());
    system(("sgdisk --backup=/tmp/gpt.bak " + DISK_ID + " > /dev/null 2>&1").c_str());
    system(("sgdisk --load-backup=/tmp/gpt.bak " + IMG + " > /dev/null 2>&1").c_str());

    std::string loop_dev = exec_and_get("losetup -f --show " + IMG);
    if (loop_dev.empty()) {
        std::cerr << "[ERROR] 回环设备挂载失败。" << std::endl;
        exit(1);
    }

    std::stringstream dm_table;
    long long current_sector = 0;

    for (const auto& p : selected_parts) {
        if (p.start > current_sector) {
            long long gap_size = p.start - current_sector;
            dm_table << current_sector << " " << gap_size << " linear " << loop_dev << " " << current_sector << "\n";
        }
        dm_table << p.start << " " << p.size << " linear " << p.path << " 0\n";
        current_sector = p.start + p.size;
    }

    if (current_sector < total_sectors) {
        long long end_gap = total_sectors - current_sector;
        dm_table << current_sector << " " << end_gap << " linear " << loop_dev << " " << current_sector << "\n";
    }

    std::string dm_cmd = "echo \"" + dm_table.str() + "\" | dmsetup create " + MAPPER_NAME;
    if (system(dm_cmd.c_str()) == 0) {
        std::string mapper_path = "/dev/mapper/" + MAPPER_NAME;
        system(("chown qemu:disk " + mapper_path).c_str());
        system(("chmod 660 " + mapper_path).c_str());
        std::cout << "成功！合成硬盘已就绪：" << mapper_path << std::endl;
        
        prompt_start_vm();
    } else {
        std::cerr << "\n映射创建失败，请检查系统日志。" << std::endl;
        stop_mount();
    }
}

int main(int argc, char* argv[]) {
    if (geteuid() != 0) {
        std::cerr << "错误: 请使用 sudo 运行此命令。\n";
        std::cerr << "用法: \n";
	std::cerr << "  sudo diskgpt -start   (启动挂载向导)\n";
        std::cerr << "  sudo diskgpt -stop    (停止并清理挂载)\n" << std::endl;
        return 1;
    }

    if (argc < 2) {
        std::cerr << "用法: \n";
        std::cerr << "  sudo diskgpt -start   (启动挂载向导)\n";
        std::cerr << "  sudo diskgpt -stop    (停止并清理挂载)\n";
        return 1;
    }

    std::string action = argv[1];
    if (action == "-start") {
        start_mount();
    } else if (action == "-stop") {
        stop_mount();
    } else {
        std::cerr << "未知参数: " << action << std::endl;
    }

    return 0;
}
