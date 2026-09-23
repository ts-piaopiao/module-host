# 契约变更流程

## 一、目的

core_contract.h 是项目唯一契约。任何对 ABI、结构体、错误码、函数签名的修改都必须走本流程，
不得在其他阶段顺手修改。

## 二、什么是契约变更

以下任何一项的修改都算契约变更：

1. CORE_ABI_VERSION 数值变化。
2. 任何错误码新增、删除或数值变化。
3. core_error、core_kind、core_pixel_format 枚举成员变化。
4. core_frame、core_intent、core_decision 字段增删、类型变化、顺序变化。
5. 宏定义变化：NAME_MAX、VERSION_MAX、NAME_BUF、VERSION_BUF、DECISION_CAPACITY、PARAM1、META_SEP。
6. plugin_* 六个入口函数签名变化。
7. CORE_API、CORE_BEGIN_DECLS、CORE_STATIC_ASSERT 行为变化。

仅仅增加注释、修改文字描述、修错别字，不算契约变更。

## 三、变更步骤

1. 起一个独立的契约变更任务，不与阶段 1 / 阶段 2 的工作混在一起。
2. 修改 core_contract.h。
3. 更新 tests/test_contract_c.c 和 tests/test_contract_cpp.cpp 中受影响的编译期断言。
4. 重跑阶段 0：C11 与 C++17 两个编译必须同时通过。
5. 重跑阶段 1：BUILD_STAGE2_PLUGINS=OFF 下 run_acceptance.ps1 -Stage stage1 必须 13/13 PASS。
6. 重跑阶段 2：BUILD_STAGE2_PLUGINS=ON 下 run_acceptance.ps1 -Stage stage2 必须 6/6 PASS，正常 plugins 输出 5 帧且退出 0。
7. 所有验证通过后再 commit，commit message 必须以 "contract:" 开头。

## 四、禁止事项

1. 不允许在阶段 1 或阶段 2 工作中顺手改 core_contract.h。
2. 不允许只改 core_contract.h 而不重跑阶段 0 / 1 / 2。
3. 不允许为了绕开某个错误桩而放宽 ABI 校验、错误码或结构体约束。
4. 不允许修改错误消息模板来绕过验收关键字检查。
5. 不允许在契约变更中删除既有错误码，只能新增；删除必须单独说明并更新所有引用。

## 五、变更记录要求

每次契约变更必须在 docs/module-host-framework.md 或单独的变更记录文件中说明：

- 变更日期
- 变更前 ABI 版本
- 变更后 ABI 版本
- 变更点列表
- 受影响的插件类型
- 是否重跑过阶段 0 / 1 / 2
