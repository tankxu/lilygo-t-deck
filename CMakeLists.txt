cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "components")

# ⚠️ 必须显式设版本号,而且必须是 x.y.z 的形式。
#
# 不设的话 ESP-IDF 会拿 git describe 的结果当 app version,在这个仓库里
# 就是一串 commit hash(如 "cad86d6")。小智的 OTA 检查会对它做
#   std::stoi(segment)   —— components/xiaozhi/src/ota.cc, ParseVersion()
# hash 解不成整数,抛 std::invalid_argument,没人 catch,直接 abort ——
# 于是【每次连上 WiFi 就重启一次】,而且现场在 C++ 异常机制里,
# 完全看不出跟版本号有关系。
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tdeck-os)
