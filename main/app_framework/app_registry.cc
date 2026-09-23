// app_registry.cc — App 静态注册表(ADR-003)
//
// 每个 app 在自己的 .cc 末尾用 TDECK_REGISTER_APP(Cls) 注册,
// launcher 只遍历这张表,不需要 #include 任何具体 app。

#include "app.h"

namespace tdeck {

AppRegistry& AppRegistry::instance()
{
    // 函数内 static:保证在第一个 registrar 的构造函数调用它时已经初始化好,
    // 不受全局静态对象初始化顺序的影响。
    static AppRegistry inst;
    return inst;
}

void AppRegistry::add(App* app)
{
    if (app) apps_.push_back(app);
}

}  // namespace tdeck
