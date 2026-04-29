#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <array>
#include <memory>
#include <map>
#include <unistd.h>

const std::string IMG = "/var/lib/libvirt/images/fake-nvme.img";
const std::string MAPPER_NAME = "win11-spliced";
const std::string CONFIG_FILE = "/etc/diskgpt.conf"; 

std::string SELECTED_DISK_ID = "";
std::string SELECTED_KNAME = "";

struct DiskInfo {
    std::string kname;
    std::string byid_path;
    std::string size;
};

struct PartitionInfo {
    int part_num;
    std::string path;
    long long start;
    long long size;
};

std::map<std::string, std::string> load_config() {
    std::map<std::string, std::string> conf;
    if (std::filesystem::exists(CONFIG_FILE)) {
        std::ifstream file(CONFIG_FILE);
        std::string line;
        while (std::getline(file, line)) {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                conf[line.substr(0, pos)] = line.substr(pos + 1);
            }
        }
    }
    return conf;
}

void save_config(const std::map<std::string, std::string>& conf) {
    std::ofstream file(CONFIG_FILE);
    for (const auto& [key, value] : conf) {
        file << key << "=" << value << "\n";
    }
}

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
    std::cout << "[INFO] 正在停止并清理挂载..." << std::endl;
    system(("dmsetup remove " + MAPPER_NAME + " 2>/dev/null").c_str());
    system("for l in $(losetup -a | grep \"fake-nvme.img\" | cut -d: -f1); do losetup -d $l 2>/dev/null; done");
    std::cout << "[OK] 清理完成！" << std::endl;
}

void check_dependencies() {
    const std::vector<std::string> deps = {"sgdisk", "losetup", "dmsetup"};
    for (const auto& dep : deps) {
        if (system(("command -v " + dep + " >/dev/null 2>&1").c_str()) != 0) {
            std::cerr << "[ERROR] 缺少必要系统依赖: " << dep << "。请先安装它。" << std::endl;
            exit(1);
        }
    }
}

void prompt_select_disk(std::map<std::string, std::string>& conf) {
    std::cout << "\n========================================================" << std::endl;
    std::cout << "           当前系统磁盘拓扑结构 (参考挂载点避开 Linux 分区)       " << std::endl;
    std::cout << "========================================================\n" << std::endl;
    system("lsblk -o NAME,MAJ:MIN,RM,SIZE,RO,TYPE,MOUNTPOINTS");
    std::cout << "\n--------------------------------------------------------" << std::endl;

    std::string last_disk = conf["LAST_DISK"];

    if (!last_disk.empty() && std::filesystem::exists(last_disk)) {
        std::error_code ec;
        std::filesystem::path target = std::filesystem::read_symlink(last_disk, ec);
        if (!ec) {
            SELECTED_KNAME = target.filename().string();
            std::cout << "\n[INFO] 发现上次配置的物理硬盘: " << SELECTED_KNAME << std::endl;
            std::cout << "是否继续使用此硬盘？(Y/n): ";
            std::string choice;
            std::getline(std::cin, choice);
            if (choice == "" || choice == "y" || choice == "Y") {
                SELECTED_DISK_ID = last_disk;
                return;
            }
        }
    }

    std::vector<DiskInfo> disks;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/disk/by-id/")) {
        std::string filename = entry.path().filename().string();
        
        if (filename.find("nvme-") != 0 && filename.find("ata-") != 0) continue;
        if (filename.find("nvme-eui.") == 0 || filename.find("wwn-") == 0) continue;
        if (filename.find("-part") != std::string::npos) continue;

        std::error_code ec;
        std::filesystem::path target = std::filesystem::read_symlink(entry.path(), ec);
        if (ec) continue;

        std::string kname = target.filename().string();
        long long size_sectors = get_sector_info(entry.path().string(), "size");
        if (size_sectors <= 0) continue;
        
        double size_gb = (size_sectors * 512.0) / (1024 * 1024 * 1024);
        char size_buf[32];
        snprintf(size_buf, sizeof(size_buf), "%.2f GB", size_gb);

        bool exists = false;
        for (const auto& d : disks) {
            if (d.kname == kname) { exists = true; break; }
        }
        if (!exists) {
            disks.push_back({kname, entry.path().string(), size_buf});
        }
    }

    std::sort(disks.begin(), disks.end(), [](const DiskInfo& a, const DiskInfo& b) {
        return a.kname < b.kname;
    });

    if (disks.empty()) {
        std::cerr << "[ERROR] 未扫描到任何 NVMe 或 SATA 物理硬盘！" << std::endl;
        exit(1);
    }

    std::cout << "\n请结合上表，选择要直通挂载的底层物理硬盘：" << std::endl;
    for (size_t i = 0; i < disks.size(); ++i) {
        std::cout << "  [" << i + 1 << "] " << disks[i].kname << " (" << disks[i].size << ")" << std::endl;
    }

    std::cout << "请输入序号: ";
    std::string input_idx;
    std::getline(std::cin, input_idx);

    try {
        int idx = std::stoi(input_idx);
        if (idx > 0 && idx <= (int)disks.size()) {
            SELECTED_DISK_ID = disks[idx - 1].byid_path;
            SELECTED_KNAME = disks[idx - 1].kname;
            conf["LAST_DISK"] = SELECTED_DISK_ID;
            std::cout << "[INFO] 已隐式绑定持久化 ID: " << SELECTED_DISK_ID << std::endl;
        } else {
            std::cerr << "[ERROR] 输入序号无效，程序退出。" << std::endl;
            exit(1);
        }
    } catch (...) {
        std::cerr << "[ERROR] 输入无效，程序退出。" << std::endl;
        exit(1);
    }
}

void prompt_start_vm(std::map<std::string, std::string>& conf) {
    std::cout << "\n======================================" << std::endl;
    std::cout << "在启动虚拟机前请手动将 \"/dev/mapper/win11-spliced\" 挂载给虚拟机\n";
    std::cout << "是否现在启动 KVM 虚拟机？(Y/n): ";
    std::string choice;
    std::getline(std::cin, choice);
    if (choice != "" && choice != "y" && choice != "Y") {
        std::cout << "[INFO] 已跳过启动虚拟机。" << std::endl;
        return;
    }

    std::cout << "--------------------------------------" << std::endl;
    std::cout << "唤醒显卡将执行以下指令：\n";
    std::cout << "  => sudo sh -c \"echo 1 > /sys/bus/pci/devices/<目标显卡PCI地址>/remove\"\n";
    std::cout << "  => sudo sh -c \"echo 1 > /sys/bus/pci/rescan\"\n";
    std::cout << "\n是否需要尝试唤醒独立显卡(Y/n): ";
    std::string gpu_choice;
    std::getline(std::cin, gpu_choice);
    
    if (gpu_choice == "" || gpu_choice == "y" || gpu_choice == "Y") {
        std::string target_gpu_pci = conf["LAST_GPU"];

        if (!target_gpu_pci.empty()) {
            std::cout << "[INFO] 发现上次唤醒的显卡: " << target_gpu_pci << std::endl;
            std::cout << "是否继续尝试唤醒此显卡？(Y/n): ";
            std::string gpu_reuse;
            std::getline(std::cin, gpu_reuse);
            if (gpu_reuse != "" && gpu_reuse != "y" && gpu_reuse != "Y") {
                target_gpu_pci = ""; 
            }
        }

        if (target_gpu_pci.empty()) {
            std::cout << "\n正在扫描系统总线中的显卡设备..." << std::endl;
            std::string gpu_list_cmd = "lspci -D | grep -iE 'vga|3d|display'";
            std::string gpu_output = exec_and_get(gpu_list_cmd);
            
            std::vector<std::string> gpus;
            std::vector<std::string> gpu_pcis;
            std::stringstream ss_gpu(gpu_output);
            std::string gpu_line;
            
            while (std::getline(ss_gpu, gpu_line)) {
                if (!gpu_line.empty()) {
                    gpus.push_back(gpu_line);
                    gpu_pcis.push_back(gpu_line.substr(0, 12)); 
                }
            }

            if (gpus.empty()) {
                std::cout << "[WARNING] 未扫描到任何显卡设备，跳过唤醒。" << std::endl;
            } else {
                for (size_t i = 0; i < gpus.size(); ++i) {
                    std::cout << "  [" << i + 1 << "] " << gpus[i] << std::endl;
                }
                std::cout << "请输入要唤醒的显卡序号: ";
                std::string input_gpu_idx;
                std::getline(std::cin, input_gpu_idx);
                try {
                    int idx = std::stoi(input_gpu_idx);
                    if (idx > 0 && idx <= (int)gpus.size()) {
                        target_gpu_pci = gpu_pcis[idx - 1];
                        conf["LAST_GPU"] = target_gpu_pci;
                    }
                } catch (...) {
                    std::cout << "[INFO] 显卡选择无效，已跳过唤醒操作。" << std::endl;
                }
            }
        }

        if (!target_gpu_pci.empty()) {
            std::cout << "[INFO] 正在下发唤醒指令至: " << target_gpu_pci << " ..." << std::endl;
            std::string cmd_remove = "sh -c \"echo 1 > /sys/bus/pci/devices/" + target_gpu_pci + "/remove\" 2>/dev/null";
            std::string cmd_rescan = "sh -c \"echo 1 > /sys/bus/pci/rescan\" 2>/dev/null";
            system(cmd_remove.c_str());
            sleep(1); 
            system(cmd_rescan.c_str());
            std::cout << "[OK] 显卡唤醒指令已下发！" << std::endl;
        }

    } else {
        std::cout << "[INFO] 已跳过显卡唤醒操作。" << std::endl;
    }
    std::cout << "--------------------------------------\n" << std::endl;

    std::string last_vm = conf["LAST_VM"];

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

    if (system("command -v virsh >/dev/null 2>&1") != 0) {
        std::cout << "[WARNING] 未检测到 virsh 命令，跳过虚拟机启动。" << std::endl;
        return;
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
        std::cout << "[WARNING] 未扫描到任何 KVM 虚拟机。" << std::endl;
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
            conf["LAST_VM"] = selected_vm;
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
    std::cout << "        DiskGPT 磁盘交互挂载工具" << std::endl;
    std::cout << "======================================\n" << std::endl;

    check_dependencies();

    auto config = load_config();

    prompt_select_disk(config);
    
    if (SELECTED_DISK_ID.empty() || !std::filesystem::exists(SELECTED_DISK_ID)) {
        std::cerr << "[ERROR] 目标硬盘失效，请检查连接。" << std::endl;
        exit(1);
    }

    std::string input_parts;
    bool use_history = false;
    std::string last_parts = config["LAST_PARTS"];

    if (!last_parts.empty()) {
        std::cout << "\n[INFO] 发现上次挂载记录，选择了分区: [" << last_parts << "]" << std::endl;
        std::cout << "是否直接使用上次的配置挂载？ (Y/n): ";
        std::string choice;
        std::getline(std::cin, choice);
        if (choice == "" || choice == "y" || choice == "Y") {
            use_history = true;
            input_parts = last_parts;
        }
    }

    if (!use_history) {
        std::cout << "\n请输入要映射的 [" << SELECTED_KNAME << "] 分区号" << std::endl;
        std::cout << "(对应最上方 lsblk 列表中的 " << SELECTED_KNAME << "p3 等，用空格分隔数字，如 '3 4'): ";
        std::getline(std::cin, input_parts);

        std::replace_if(input_parts.begin(), input_parts.end(), [](char c) {
            return !std::isdigit(c) && c != ' ';
        }, ' ');

        config["LAST_PARTS"] = input_parts;
        std::cout << "[INFO] 分区选择已保存，下次可直接加载。" << std::endl;
    }

    std::stringstream ss_input(input_parts);
    std::vector<PartitionInfo> selected_parts;
    int p_num;
    while (ss_input >> p_num) {
        PartitionInfo pi;
        pi.part_num = p_num;
        pi.path = SELECTED_DISK_ID + "-part" + std::to_string(p_num);
        pi.start = get_sector_info(pi.path, "start");
        pi.size = get_sector_info(pi.path, "size");
        
        if (pi.size == 0) {
            std::cerr << "[WARNING] 无法读取分区 " << p_num << " ，该分区可能不存在，将跳过。" << std::endl;
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
    long long total_sectors = get_sector_info(SELECTED_DISK_ID, "size");
    system("mkdir -p /var/lib/libvirt/images/");
    system(("truncate -s " + std::to_string(total_sectors * 512) + " " + IMG).c_str());
    system(("sgdisk --backup=/tmp/gpt.bak " + SELECTED_DISK_ID + " > /dev/null 2>&1").c_str());
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
        
        prompt_start_vm(config);

        save_config(config);
    } else {
        std::cerr << "\n映射创建失败，请检查系统日志。" << std::endl;
        stop_mount();
    }
}

void automatic_start() {
    std::cout << "[INFO] 正在执行全自动静默挂载流程..." << std::endl;
    check_dependencies();
    auto config = load_config();

    SELECTED_DISK_ID = config["LAST_DISK"];
    if (SELECTED_DISK_ID.empty() || !std::filesystem::exists(SELECTED_DISK_ID)) {
        std::cerr << "[ERROR] 缺少有效的硬盘配置，请先运行 sudo diskgpt -start 进行一次完整配置。" << std::endl;
        exit(1);
    }

    std::string input_parts = config["LAST_PARTS"];
    if (input_parts.empty()) {
        std::cerr << "[ERROR] 缺少有效的分区配置，请先运行 sudo diskgpt -start 进行一次完整配置。" << std::endl;
        exit(1);
    }

    std::stringstream ss_input(input_parts);
    std::vector<PartitionInfo> selected_parts;
    int p_num;
    while (ss_input >> p_num) {
        PartitionInfo pi;
        pi.part_num = p_num;
        pi.path = SELECTED_DISK_ID + "-part" + std::to_string(p_num);
        pi.start = get_sector_info(pi.path, "start");
        pi.size = get_sector_info(pi.path, "size");
        if (pi.size > 0) {
            selected_parts.push_back(pi);
        }
    }

    if (selected_parts.empty()) {
        std::cerr << "[ERROR] 没有成功读取到分区信息，请检查硬盘状态。" << std::endl;
        exit(1);
    }

    std::sort(selected_parts.begin(), selected_parts.end(), [](const PartitionInfo& a, const PartitionInfo& b) {
        return a.start < b.start;
    });

    stop_mount(); 

    long long total_sectors = get_sector_info(SELECTED_DISK_ID, "size");
    system("mkdir -p /var/lib/libvirt/images/");
    system(("truncate -s " + std::to_string(total_sectors * 512) + " " + IMG).c_str());
    system(("sgdisk --backup=/tmp/gpt.bak " + SELECTED_DISK_ID + " > /dev/null 2>&1").c_str());
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
        std::cout << "[OK] 合成硬盘已就绪：" << mapper_path << std::endl;

        // 自动重置显卡
        if (!config["LAST_GPU"].empty()) {
            std::cout << "[INFO] 正在尝试唤醒显卡 " << config["LAST_GPU"] << " ..." << std::endl;
            std::string cmd_remove = "sh -c \"echo 1 > /sys/bus/pci/devices/" + config["LAST_GPU"] + "/remove\" 2>/dev/null";
            std::string cmd_rescan = "sh -c \"echo 1 > /sys/bus/pci/rescan\" 2>/dev/null";
            system(cmd_remove.c_str());
            sleep(1); 
            system(cmd_rescan.c_str());
        }

        // 自动启动虚拟机
        if (!config["LAST_VM"].empty()) {
            std::cout << "[INFO] 正在启动虚拟机 " << config["LAST_VM"] << "..." << std::endl;
            system(("virsh start " + config["LAST_VM"]).c_str());
        }

    } else {
        std::cerr << "\n[ERROR] 映射创建失败，请检查系统日志。" << std::endl;
        stop_mount();
    }
}

int main(int argc, char* argv[]) {
    if (geteuid() != 0) {
        std::cerr << "错误: 请使用 sudo 运行此命令。\n";
        std::cerr << "用法: \n";
        std::cerr << "  sudo diskgpt -start       (启动交互式挂载向导)\n";
        std::cerr << "  sudo diskgpt -automatic   (按上次配置一键全自动执行)\n";
        std::cerr << "  sudo diskgpt -stop        (停止并清理挂载)\n" << std::endl;
        return 1;
    }

    if (argc < 2) {
        std::cerr << "用法: \n";
        std::cerr << "  sudo diskgpt -start       (启动交互式挂载向导)\n";
        std::cerr << "  sudo diskgpt -automatic   (按上次配置一键全自动执行)\n";
        std::cerr << "  sudo diskgpt -stop        (停止并清理挂载)\n";
        return 1;
    }

    std::string action = argv[1];
    if (action == "-start") {
        start_mount();
    } else if (action == "-automatic") {
        automatic_start();
    } else if (action == "-stop") {
        stop_mount();
    } else {
        std::cerr << "未知参数: " << action << std::endl;
    }

    return 0;
}
