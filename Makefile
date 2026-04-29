CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2
TARGET = diskgpt

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
CONFIG_FILE = /etc/diskgpt.conf

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): main.cpp
	@echo "正在编译 C++ 源码..."
	$(CXX) $(CXXFLAGS) -o $(TARGET) main.cpp

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@echo "正在安装工具到 $(BINDIR) ..."
	install -m 0755 $(TARGET) $(BINDIR)/$(TARGET)
	@echo "======================================"
	@echo "安装完成！"
	@echo "现在你可以随时在终端使用以下命令："
	@echo "  sudo diskgpt -start       (交互式配置与挂载)"
	@echo "  sudo diskgpt -automatic   (一键全自动静默挂载)"
	@echo "  sudo diskgpt -stop        (停止并清理环境)"
	@echo "======================================"

uninstall:
	@echo "正在移除程序与配置文件 ..."
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(CONFIG_FILE)
	@echo "卸载完成！环境已彻底清理。"
