# RMDB 数据库管理系统 - 项目架构文档

## 项目目录结构

```
rmdb/
├── CMakeLists.txt                    # 顶层 CMake 配置
├── ARCHITECTURE.md                   # 本文件：项目架构说明
├── src/
│   ├── rmdb.cpp                      # 服务器主入口：启动、连接管理、SQL分发
│   ├── defs.h                        # 全局类型定义 (Rid, ColType, RecScan)
│   ├── errors.h                      # 错误异常类定义
│   ├── portal.h                      # Portal: 执行计划 → 执行器树的转换与运行
│   ├── record_printer.h              # 输出格式化工具
│   │
│   ├── common/                       # 通用基础设施
│   │   ├── config.h                  #   全局常量配置 (PAGE_SIZE, BUFFER_POOL_SIZE等)
│   │   ├── common.h                  #   公共数据结构 (Value, Condition, SetClause, TabCol)
│   │   ├── context.h                 #   执行上下文 (Context: 事务、锁、日志、输出缓冲)
│   │   └── exception.h               #   通用异常定义
│   │
│   ├── parser/                       # SQL 解析层
│   │   ├── ast.h / ast.cpp           #   抽象语法树 (AST) 节点定义
│   │   ├── yacc.y / yacc.tab.cpp     #   Yacc 语法分析器
│   │   ├── lex.l / lex.yy.cpp        #   Lex 词法分析器
│   │   ├── parser.h                  #   Parser 接口
│   │   └── parser_defs.h             #   Parser 相关定义
│   │
│   ├── analyze/                      # 语义分析层
│   │   ├── analyze.h                 #   Analyze 类接口
│   │   └── analyze.cpp               #   语义分析: AST → Query (表名校验、列名校验、条件解析)
│   │
│   ├── optimizer/                    # 查询优化层
│   │   ├── plan.h                    #   执行计划节点定义 (ScanPlan, JoinPlan, DMLPlan, DDLPlan等)
│   │   ├── planner.h / planner.cpp   #   Planner: Query → Plan (逻辑优化 + 物理优化 + 计划生成)
│   │   └── optimizer.h               #   Optimizer: 入口分发 (DDL/DML/Select路由)
│   │
│   ├── execution/                    # 执行引擎层
│   │   ├── executor_abstract.h       #   执行器抽象基类 (beginTuple, nextTuple, Next, is_end)
│   │   ├── executor_seq_scan.h       #   ★ 顺序扫描执行器 (带条件过滤)
│   │   ├── executor_index_scan.h     #   索引扫描执行器
│   │   ├── executor_insert.h         #   ★ 插入执行器 (支持事务写记录)
│   │   ├── executor_delete.h         #   ★ 删除执行器 (支持事务写记录)
│   │   ├── executor_update.h         #   ★ 更新执行器 (支持事务写记录)
│   │   ├── executor_projection.h     #   ★ 投影执行器 (列裁剪+偏移重映射)
│   │   ├── executor_nestedloop_join.h #  ★ 嵌套循环连接执行器
│   │   ├── execution_sort.h          #   排序执行器 (框架提供，待实现)
│   │   ├── execution_manager.h/.cpp  #   QlManager: select_from / run_dml / run_cmd_utility
│   │   ├── execution_defs.h          #   执行层依赖头
│   │   └── execution_common.h        #   执行层公共定义
│   │
│   ├── storage/                      # 存储引擎层
│   │   ├── disk_manager.h/.cpp       #   ★ 磁盘管理器 (文件操作、页面读写)
│   │   ├── buffer_pool_manager.h/.cpp #  ★ 缓冲池管理器 (页面置换、内外存交换)
│   │   └── page.h                    #   页面数据结构
│   │
│   ├── replacer/                     # 缓冲池替换策略
│   │   ├── replacer.h                #   Replacer 抽象接口
│   │   └── lru_replacer.h/.cpp       #   ★ LRU 替换策略实现
│   │
│   ├── record/                       # 记录管理层
│   │   ├── rm_defs.h                 #   记录层定义 (RmFileHdr, RmPageHdr, RmRecord)
│   │   ├── rm_file_handle.h/.cpp     #   ★ 表文件句柄 (记录CRUD、页面管理)
│   │   ├── rm_scan.h/.cpp            #   ★ 表记录扫描器 (Bitmap遍历)
│   │   ├── rm_manager.h              #   记录管理器 (文件创建/打开/关闭)
│   │   ├── rm.h                      #   记录层聚合头文件
│   │   └── bitmap.h                  #   Bitmap 工具类
│   │
│   ├── system/                       # 系统管理层
│   │   ├── sm_defs.h                 #   系统层定义 (ColType 枚举)
│   │   ├── sm_meta.h                 #   元数据结构 (ColMeta, TabMeta, IndexMeta, DbMeta)
│   │   ├── sm_manager.h/.cpp         #   ★ 系统管理器 (建库/建表/删表/元数据持久化)
│   │   └── sm.h                      #   系统层聚合头文件
│   │
│   ├── index/                        # 索引层 (B+树)
│   │   ├── ix_defs.h                 #   索引层定义
│   │   ├── ix_index_handle.h/.cpp    #   索引文件句柄
│   │   ├── ix_scan.h/.cpp            #   索引扫描器
│   │   ├── ix_manager.h              #   索引管理器
│   │   └── ix.h                      #   索引层聚合头文件
│   │
│   ├── transaction/                  # 事务管理层
│   │   ├── txn_defs.h                #   ★ 事务定义 (TransactionState, WriteRecord, LockDataId)
│   │   ├── transaction.h             #   ★ Transaction 类 (write_set_, lock_set_, undo_logs)
│   │   ├── transaction_manager.h/.cpp #  ★ 事务管理器 (begin/commit/abort + 写记录回滚)
│   │   ├── watermark.h/.cpp          #   MVCC 水位线管理
│   │   └── concurrency/              #   并发控制
│   │       ├── lock_manager.h/.cpp   #     锁管理器 (当前为空实现)
│   │       └── ...
│   │
│   └── recovery/                     # 恢复层
│       ├── log_defs.h                #   日志定义
│       ├── log_manager.h/.cpp        #   日志管理器 (当前为空实现)
│       └── log_recovery.h/.cpp       #   恢复管理器 (当前为空实现)
│
└── build/                            # 构建输出目录
    └── bin/
        ├── rmdb                      #   数据库服务器可执行文件
        └── unit_test                 #   单元测试可执行文件
```

## 我们修改/实现的文件标记 (★)

| 文件 | 修改内容 | 对应题目 |
|------|----------|----------|
| `storage/disk_manager.cpp` | 实现 create/open/close/destroy_file, read/write_page | 题目一 |
| `replacer/lru_replacer.cpp` | 实现 LRU victim/pin/unpin | 题目一 |
| `storage/buffer_pool_manager.cpp` | 实现 new/fetch_page, find_victim, update/unpin/delete/flush | 题目一 |
| `record/rm_file_handle.cpp` | 实现 get/insert/delete/update_record, RmScan | 题目一 |
| `system/sm_manager.cpp` | 实现 open_db/close_db/drop_table | 题目二 |
| `analyze/analyze.cpp` | 补全 UpdateStmt 语义分析、check_column | 题目二 |
| `execution/executor_seq_scan.h` | 实现顺序扫描 + 条件过滤 | 题目二 |
| `execution/executor_projection.h` | 实现列投影 + 偏移重映射 | 题目二 |
| `execution/executor_delete.h` | 实现删除 + 事务写记录 | 题目二/八 |
| `execution/executor_update.h` | 实现更新 + 事务写记录 | 题目二/八 |
| `execution/executor_nestedloop_join.h` | 实现嵌套循环连接 | 题目二 |
| `execution/executor_index_scan.h` | 索引扫描 (回退到顺序扫描) | 题目二 |
| `transaction/transaction_manager.cpp` | 实现 begin/commit/abort + 写记录回滚 | 题目二/八 |

## 执行流程

```
SQL 字符串
  ↓ (Lex + Yacc)
AST 语法树
  ↓ (Analyze::do_analyze)
Query 查询对象 (tables, cols, conds, values, set_clauses)
  ↓ (Optimizer::plan_query → Planner::do_planner)
Plan 执行计划树 (ScanPlan, JoinPlan, DMLPlan, DDLPlan)
  ↓ (Portal::start → convert_plan_executor)
Executor 执行器树 (SeqScanExecutor, ProjectionExecutor, ...)
  ↓ (Portal::run → QlManager)
执行结果 → output.txt + 客户端
```
