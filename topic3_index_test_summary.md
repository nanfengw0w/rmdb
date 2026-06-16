# RMDB 题目三唯一索引测试经验与项目介绍

## 项目介绍

RMDB 是一个教学型关系数据库系统，整体结构接近传统数据库内核，主要模块包括：

- `record`：记录管理，负责表文件、记录插入、删除、更新和顺序扫描。
- `index`：B+ 树索引管理，负责索引文件创建、打开、插入、删除、查找和叶子扫描。
- `system`：系统管理，负责数据库、表、索引的元数据维护。
- `optimizer`：查询计划生成，决定使用顺序扫描还是索引扫描。
- `execution`：执行器层，负责 `insert`、`delete`、`update`、`select` 等语句执行。
- `parser/analyze`：SQL 解析和语义检查。

第三题唯一索引的核心目标是让 `create index` 后数据库能真正维护唯一索引，并且让索引扫描结果和普通顺序扫描结果完全一致。评测重点不是只看索引能不能创建，而是检查建索引前后的同一条 `select` 输出是否一致。

## 唯一索引需要覆盖的功能

唯一索引相关改动主要分布在以下位置：

- `src/system/sm_manager.cpp`
  - `create_index` 创建索引文件、更新表元数据、打开 index handle。
  - 扫描已有表数据，把已有记录插入 B+ 树。
  - 如果已有数据在索引列上重复，必须拒绝创建索引并清理已创建的索引文件。
  - `drop_index` 关闭并销毁索引文件，同时更新元数据。

- `src/execution/executor_insert.h`
  - 插入前检查所有唯一索引冲突。
  - 插入记录后同步插入所有索引项。

- `src/execution/executor_delete.h`
  - 删除记录时同步删除对应索引项。

- `src/execution/executor_update.h`
  - 更新索引列前检查新 key 是否冲突。
  - 成功更新时保持记录和索引一致。
  - 更新失败不能破坏原记录和原索引。

- `src/execution/executor_index_scan.h`
  - 必须真正使用 `IxScan` / `IxIndexHandle`。
  - 建索引后 `select` 输出必须和未建索引时一致。
  - 单列索引可使用精确 key 和范围 bound。
  - 复合索引优先保证正确性：扫描完整索引叶子链，再按索引 key 和 record 条件过滤，最后按 RID 排序输出。

- `src/optimizer/planner.cpp`
  - planner 在有可用索引时选择 `IndexScan`。
  - 但优化器选择索引后，执行器必须保证结果和 `SeqScan` 一致。

## 本次主要问题

### 1. B+ 树页号分配覆盖 root page

索引文件初始化时，`fd2pageno` 如果从 `IX_INIT_NUM_PAGES - 1` 开始，会导致第一次分裂创建新页时复用 root page 的页号。结果是 B+ 树 root 被覆盖，后续 `get_value` 或范围扫描会出现 key/rid 错配。

修复经验：

- 初始索引文件已有 page 0、page 1、page 2。
- 新页分配应从 `IX_INIT_NUM_PAGES` 开始。
- 修复后需要用大量插入触发叶子分裂和内部节点分裂。

### 2. B+ 树节点容量 off-by-one

节点实际预留了一个临时溢出槽用于插入后分裂，但稳定容量不能把这个临时槽也当作正常最大容量。否则插入时会写越界，导致 key 和 rid 对不上。

修复经验：

- `get_max_size()` 应返回稳定容量 `btree_order_`。
- 插入超过稳定容量后立刻 split。
- 测试时必须插入足够多的数据触发多次 split。

### 3. 复合索引前缀 bound 漏行

复合索引 `(a,b)` 如果只用第一列 `a` 构造 lower bound，并把后续列默认填 `0`，会漏掉后缀小于 0 的 key。

典型失败场景：

```sql
create index t(a, b);
select * from t where a = 0;
```

如果真实数据包含 `(0,-20)` 到 `(0,20)`，错误 bound `(0,0)` 会直接跳过 `(0,-20)` 到 `(0,-2)`。

最终处理经验：

- 单列索引继续使用精确查找和范围 bound。
- 复合索引为了保证评测正确性，直接扫描完整索引叶子链，再过滤条件。
- 这样仍然是真正使用索引文件，不是假装成全表扫。
- 输出按 RID 排序，和顺序扫描保持一致。

## 测试经验

### 1. 建索引前后必须对比同一条 select

评测报错的核心是：

```text
after created index your 'select' answer not match expected output
```

所以测试不能只看 `create index` 成功，也不能只看 `select` 能跑。必须记录建索引前的标准输出，再建索引后跑同样 SQL，逐字节比较结果。

推荐测试流程：

```sql
create table t(a int, b int, c int);
insert into t values (...);

select * from t where a = 0;
create index t(a, b);
select * from t where a = 0;
```

两次 `select` 的结果、顺序、列格式都必须一致。

### 2. 单列索引测试点

单列索引至少覆盖：

```sql
select * from t where a = 1;
select * from t where a < 10;
select * from t where a <= 10;
select * from t where a > 10;
select * from t where a >= 10;
select * from t where a > 5 and a < 20;
select * from t where a >= 5 and a <= 20;
```

还要覆盖：

- 查询不存在的 key。
- 查询最小 key。
- 查询最大 key。
- 插入顺序打乱，不能只测有序插入。
- 数据量要足够大，必须触发 B+ 树 split。

### 3. 复合索引测试点

复合索引不能只测完整 key 等值：

```sql
select * from t where a = 1 and b = 2;
```

还必须测前缀和范围：

```sql
select * from t where a = 0;
select * from t where a = 0 and b < 0;
select * from t where a = 0 and b >= -4 and b <= 8;
select * from t where a >= -5 and a <= 5;
select * from t where a >= -5 and a <= 5 and b >= -8 and b <= 8;
```

复合索引容易漏掉的边界：

- 后缀列是负数。
- 第一列是字符串。
- 第一列是 float。
- where 条件顺序和索引列顺序不同。
- 只约束第二列，但 planner 误选复合索引。
- `!=` 条件和范围条件组合。

### 4. 唯一性测试点

唯一索引必须覆盖：

```sql
create table t(a int, b int);
insert into t values (1, 10);
insert into t values (1, 20);
create index t(a);
```

期望：创建失败，因为已有重复 `a = 1`。

插入重复：

```sql
create table t(a int, b int);
create index t(a);
insert into t values (1, 10);
insert into t values (1, 20);
```

期望：第二次 insert 失败，原数据和索引不被破坏。

更新成重复：

```sql
create table t(a int, b int);
create index t(a);
insert into t values (1, 10);
insert into t values (2, 20);
update t set a = 1 where b = 20;
```

期望：update 失败，`a=2,b=20` 这条记录保持原样，索引也保持原样。

删除后再插入：

```sql
delete from t where a = 1;
insert into t values (1, 30);
```

期望：删除同步清理索引后，重新插入同 key 可以成功。

### 5. 直接写 C++ 回归比手工 SQL 更容易定位

手工 SQL 适合最终验收，但定位 B+ 树问题时，直接写临时 C++ 测试更快：

- 用 `SeqScanExecutor` 收集标准答案。
- 用 `IndexScanExecutor` 收集索引答案。
- 比较完整结果向量。
- 直接检查 `IxScan` 叶子链 key。
- 直接调用 `IxIndexHandle::get_value` 验证 key/rid 是否一致。

这类测试可以快速发现：

- key 存在但 `get_value` 查不到。
- key 查到了但 rid 指向错误记录。
- 范围扫描漏前半段或漏后半段。
- 输出顺序和顺序扫描不一致。

## 推荐回归命令

项目环境中 `cmake` 不一定存在时，可以直接使用已有 build 目录：

```bash
make -C build -j
./build/bin/unit_test
./build/bin/test_parser
```

本次额外使用的临时回归包括：

```bash
/tmp/index_direct_test
/tmp/index_multi_direct_test
/tmp/index_multi_mixed_test
```

覆盖范围：

- 单列 int 索引等值和范围查询。
- 复合 int 索引前缀、范围、负数后缀、完整 key。
- 复合 char/int 和 float/int 索引。
- B+ 树大量随机插入后的 split 稳定性。
- `get_value` 返回 rid 是否对应正确记录。

## 最重要的经验

1. 索引题最关键的不是“使用了索引”，而是“使用索引后结果完全不变”。
2. 建索引前后的同一条 `select` 必须逐行一致，包括顺序。
3. 单列索引正确不代表复合索引正确。
4. 复合索引前缀扫描很容易被后缀默认值坑到，尤其是负数、float 和字符串。
5. B+ 树 bug 经常表现为 select 结果错，但根因可能是页号、容量、split 或 rid 错配。
6. DML 维护索引必须先检查唯一冲突，再改数据和索引，失败时不能留下半更新状态。
7. 测试数据必须随机插入，并且数量要足够触发多次分裂。
8. 评测里看到 `after created index select answer not match`，第一反应应是对比 `SeqScan` 和 `IndexScan` 的完整结果，而不是只看 SQL 是否执行成功。
