# 版本记录

## 在线测试结果汇总

## 第十题静态检查点修复记录

### 2026-06-29 checkpoint 正常运行阶段崩溃修复
- **线上基线**: 7.30 分，仅 `crash_recovery_with_checkpoint` 失败；无 checkpoint 的恢复一致性已通过，checkpoint 用例在正常运行阶段 server stops running。
- **定位**:
  1. `DiskManager` 使用 `lseek + read/write`，同一 fd 多线程并发时文件偏移会互相覆盖；checkpoint 刷大量脏页时容易把事务页读写到错误位置。
  2. `create static_checkpoint` 没有阻止新事务穿过 checkpoint，也没有等待当前活跃事务结束，不满足静态检查点语义。
  3. checkpoint 日志记录携带 active txn 列表，`LogManager::add_log_to_buffer()` 未处理单条日志大于 `LOG_BUFFER_SIZE` 的情况，存在日志缓冲区越界风险。
  4. 同一连接若在显式事务中执行 checkpoint，等待 active txn 清空会等到自己，存在自死锁风险。
- **改动**:
  1. 数据页和日志读取改为 `pread/pwrite` 定位 I/O，日志追加加互斥保护。
  2. `LogManager` 增加 checkpoint 屏障：checkpoint 开始后阻塞新事务 begin，等待 active txn 清空，再写 checkpoint 和刷脏页。
  3. 大日志记录直接写盘，避免越界；恢复扫描不再用 `LOG_BUFFER_SIZE` 错误限制单条日志长度。
  4. checkpoint 命令清空响应缓冲，避免网络响应带旧数据尾巴。
  5. checkpoint 前若当前连接仍有未完成事务，先 abort 并清空当前 txn id，避免 checkpoint 等待自身。
- **预期**: 修复 checkpoint 正常运行阶段崩溃，并使 checkpoint 记录对应真正无活跃事务的静态检查点。

## 第九题 SI/SER 修复记录

### 2026-06-28 TupleReconstructTest 修复尝试
- **线上基线**: 19.20 分，仅剩 `si/TupleReconstructTest` mismatch。
- **改动**:
  1. `VersionManager::get_visible_data()` / `get_read_committed_data()` 不再返回版本链内部 `RmRecord*` 裸指针，改为在版本管理器分片锁内复制出 `std::unique_ptr<RmRecord>`，避免并发 abort/清理版本链时读线程拿到悬空历史元组。
  2. `SmManager::drop_table()` / `close_db()` 清理对应 fd 的版本链，避免 drop/recreate table 后 OS 复用 fd，旧表的 MVCC 版本污染新表同 page/slot 的记录。
- **本地验证**:
  - `make -C build -j` 通过。
  - `./build/bin/unit_test` 5/5 通过。
  - socket 级 SI 回归通过：同一事务多次 update 后旧快照全字段重构、分段提交后的中间快照重构、索引键更新重构、delete 后旧快照读、delete 后重插同 key、abort 混合写恢复、写写冲突只输出一次 `abort`。
  - drop/recreate 表后新表记录可见性通过。
- **风险说明**: 这是生命周期和并发安全修复，不改变 MVCC 可见性判定规则；若线上仍失败，下一步重点查隐藏用例的输出协议或特殊 SQL 触发路径。

| Commit | 描述 | 结果 | tpmC | 备注 |
|--------|------|------|------|------|
| 338e057 | VersionManager分片锁+解析器优化 | AC | 3.17 | 首次AC |
| d14519d | 回退过度优化 | AC | 3.17 | - |
| 584f000 | 跳过check_logical_key_write_conflict | **WA** | - | 一致性失败 |
| a329c69 | 回退 | AC | 3.17 | - |
| ccdb3c5 | 解析器do_analyze移出锁 | **WA** | - | 一致性失败 |
| 9fa2ec2 | 回退解析器优化 | AC | 3.17 | - |
| 9d4a3c0 | VersionManager per-txn key tracking | **WA** | - | 死锁/卡住 |
| b4c9070 | 回退VersionManager优化 | AC | 3.17 | - |
| efb2132 | VersionManager优化+修复锁 | **WA** | - | 死锁/卡住 |
| 44de123 | 回退 | AC | 3.50 | - |
| 5850d1f | WAL跳过空缓冲区刷盘 | **WA** | - | 一致性失败 |
| 52f25cd | 回退WAL | AC | 3.50 | - |
| bbb3d2e | WAL跳过空缓冲区刷盘(再次) | **WA** | - | 一致性失败 |
| ea3adc2 | 回退WAL | AC | 3.50 | - |
| faef601 | 跳过check_logical_key_write_conflict | **WA** | - | 一致性失败 |
| da4a1e0 | 回退 | AC | 3.50 | - |
| 0991ad1 | 回退缓冲池改动 | AC | 3.50 | 已知线上AC基线 |
| 05f90ef | 事务控制快路径+按写集提交版本 | **WA** | - | Phase 3 Post-transaction consistency validation失败 |
| 44ecb18 | 回退05f90ef | 待线上验证 | - | 回到0991ad1源码状态 |
| c2520ea | 聚合/ORDER BY扫描热路径优化 | **WA** | - | Phase 3 Post-transaction consistency validation失败 |
| 4b184cb | 回退c2520ea | 待线上验证 | - | 回到44ecb18源码状态 |
| 39200e1 | 仅提高server listen backlog | AC但性能下降 | 2.33 | 只改MAX_CONN_LIMIT 8->128，低于3.50基线 |
| 16:30提交版 | 回退listen backlog到8 + 复合索引精确/前缀扫描 + 增强本地门禁 | **WA** | - | Phase 3 Post-transaction consistency validation失败，复合索引扫描优化不可靠 |
| 当前未提交 | 回退复合索引扫描优化，仅保留listen backlog回退 + 增强本地门禁 | 本地通过 | - | 本地门禁已修正验证隔离级别，能抓复合索引范围漏读类错误；等待线上验证 |
| 当前未提交 | 安全性能优化包：SeqScan缓存+列偏移预计算+pread/pwrite+SI单列IndexScan | 本地通过 | - | 两档门禁+unit_test通过；等待线上验证 |

## 错因分析

### 1. 解析器优化 (ccdb3c5) - WA
- **改动**: 把 `do_analyze()` 移出 `pthread_mutex_lock`
- **错因**: `do_analyze()` 访问共享状态 `sm_manager->db_`，多线程并发访问导致数据竞争
- **教训**: 不能把访问共享状态的代码移出锁

### 2. VersionManager优化 (9d4a3c0, efb2132) - WA
- **改动**: 用 `txn_keys_` 跟踪每个事务修改的键，commit/abort 只遍历这些键
- **错因**: 事务卡住，只有1个完成，199个挂起
- **分析**: 可能是 `txn_keys_` 没有正确记录所有键，导致 commit 漏掉某些版本条目
- **教训**: VersionManager 的 commit/abort 逻辑不能简化

### 3. WAL优化 (5850d1f, bbb3d2e) - WA
- **改动**: `flush_log_to_disk` 跳过空缓冲区刷盘
- **错因**: 可能影响 `persist_lsn_` 更新，或影响崩溃恢复
- **教训**: WAL 的任何改动都可能影响崩溃恢复一致性

### 4. check_logical_key_write_conflict跳过 (faef601) - WA
- **改动**: 当第一列有索引时跳过此检查
- **错因**: 这个检查不是冗余的！它检测的是"逻辑键写冲突"
- **详细分析**: 
  - orders 表主键是 (o_id, o_d_id, o_w_id)，第一列是 o_id
  - T1 插入 order (100, 1, 1)，T2 插入 order (100, 2, 1)
  - 两个记录主键不同，不冲突于唯一索引
  - 但它们共享 o_id=100，`check_logical_key_write_conflict` 检测到 T1 的未提交写入
  - 这保证了事务按正确顺序执行，防止"幻读"类异常
- **教训**: 不能跳过这个检查，它与 `check_unique_conflict` 检测不同类型的冲突

## 结论

所有优化都导致了 WA。根本原因是：
1. **事务核心逻辑不能改** - commit/abort/WAL 的任何改动都可能破坏一致性
2. **冲突检测不能跳过** - 每个检查都有其存在的理由
3. **共享状态不能移出锁** - 多线程并发访问需要严格的锁保护

### 5. 缓冲池增大 (4ddc68b) - AC 但性能下降
- **改动**: BUFFER_POOL_SIZE 从 65536 (256MB) 增到 131072 (512MB)
- **结果**: AC, tpmC=3.33 (从3.50下降), 内存从0.46GB涨到0.72GB
- **分析**: 瓶颈不在磁盘I/O，增大缓冲池无帮助反而有害
- **教训**: 不能盲目增大缓冲池

### 6. 事务控制快路径+按写集提交版本 (05f90ef) - WA
- **改动**:
  - `begin/commit/abort/rollback` 绕过 parser，直接走事务控制逻辑。
  - `VersionManager` 增加单记录提交接口，commit 按 write set 标记版本。
  - 连接上限从 8 提到 128。
- **本地结果**: 编译、题4/5/6、恢复脚本和短压测通过，短压测约224 tpmC/0 abort。
- **线上结果**: Phase 1/2 全过，但 Phase 3 压测后 `Post-transaction consistency validation` 失败。
- **错因判断**: 本地短压测没有覆盖线上 TPC-C 的一致性检查场景；按 write set 提交版本或事务控制快路径仍可能改变高并发事务后可见性/清理时序。
- **教训**: 只要 Phase 3 一致性失败，就不能保留。之后不要再用“本地短压测通过”替代线上一致性验证。

### 7. 聚合/ORDER BY扫描热路径优化 (c2520ea) - WA
- **改动**: 单表 `handle_aggregate` 预解析 WHERE，并尝试用索引作为聚合/ORDER BY/LIMIT 的候选 RID 来源。
- **线上结果**: Phase 1/2 全过，但 Phase 3 压测后 `Post-transaction consistency validation` 失败。
- **错因判断**: Phase 3 的一致性检查会执行聚合/排序类 SQL；改动虽然不碰事务写路径，但改变了检查 SQL 的读路径，可能漏读/重复读 MVCC 下的索引候选，导致一致性检查结果不匹配。
- **本地复现**: 新增 `bench_tpcc_consistency.py` 后，`c2520ea` 会稳定失败在 `ORDER BY a,b LIMIT 1` 探针上。该版本只按第一排序列 `a` 的索引提前停止，返回 `(1,5)`，正确结果应为 `(1,3)`。
- **教训**: 不能改一致性检查可能依赖的聚合/ORDER BY扫描路径，除非能完整复现评测一致性 SQL。

## 当前状态

- **线上AC基线**: 0991ad1 (回退缓冲池改动), tpmC=3.50
- **当前代码**: 基于 39200e1，已把 server listen backlog 从 128 恢复到 8；等待线上验证是否回到 3.50 基线。
- **已确认错误版本**: 05f90ef、c2520ea，均为 Phase 3 一致性失败，不能再作为优化基线
- **本地验证**: `make -C build -j`、`unit_test`、题4/5/6脚本、基础/检查点恢复脚本通过。
- **新增本地门禁**: `bench_tpcc_consistency.py`
  - 坏版验证: `c2520ea` + `python3 bench_tpcc_consistency.py -w 1 -d 0 --warehouses 1 --items 20 --customers 5 --initial-orders 5 --new-order-start 3 --index-order tpcc` 失败，抓到 `ORDER BY a,b LIMIT 1` 错误。
  - 好版验证: `39200e1` 同命令通过；`python3 bench_tpcc_consistency.py -w 6 -d 20 --warehouses 2 --index-order tpcc` 也通过。
  - 脚本采用 TPC-C 主键索引顺序、事务后分段快照建模、聚合 SQL 交叉校验、stock/order_line 一致性检查，避免大结果集被服务端发送缓冲截断。
- **本地门禁修正**:
  - 旧脚本的验证连接也执行了 `SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION`，而 `portal.h` 在 SI 下会强制 Scan 走 SeqScan，导致脚本绕开 READ_COMMITTED 下的 `IndexScanExecutor`，抓不到 16:30 类 IndexScan 错误。
  - 当前脚本保留 SI 压测连接，但 consistency validation 改用默认隔离级别，贴近线上 Phase 3 的事务后验证路径。
  - 新增 `composite_probe`：覆盖复合索引 `(a,b,c)` 的前缀等值 + 后续列范围、更新索引列、删除索引键后再 SELECT 的行集合校验。
  - 已用临时坏补丁验证：复合索引范围漏读时，脚本会失败在 `composite probe select closed-open range` 等检查；回退坏补丁后同命令通过。
- **本次改动**:
  - `MAX_CONN_LIMIT` 从 128 恢复到 8，撤销 39200e1 的负收益。
  - 已回退 `IndexScanExecutor` 复合索引精确/前缀扫描优化。线上 16:30 提交证明该优化会导致 Phase 3 事务后数据一致性失败。
  - 新增 `bench_tpcc_consistency.py`，作为性能优化前后的本地一致性门禁。
- **最新教训**:
  - 本地脚本最初没抓到 16:30 WA，是因为验证阶段误设 SI，绕过了 IndexScan。这类脚本如果不区分压测隔离级别和验证隔离级别，就不能作为性能优化门禁。
  - 涉及 `IndexScanExecutor` 候选 RID 裁剪的优化，必须先让 `bench_tpcc_consistency.py` 在默认隔离级别验证下通过，并额外用故意坏补丁证明脚本能失败。
- **后续方向**: 不能继续碰 WAL、冲突检测、VersionManager 提交流程、显式事务控制快路径、聚合/ORDER BY扫描路径。39200e1 没有性能收益，下一步应先增强本地 Phase 3 consistency 脚本，再回到 0991ad1/44ecb18 基线做可验证的小优化。

### 8. 安全性能优化包 (当前) - 待线上验证
- **改动**:
  1. **SeqScanExecutor 缓存记录**: `beginTuple()/nextTuple()` 缓存匹配记录，`Next()` 直接返回，避免同一条记录调用两次 `get_record()`。
  2. **预计算列元数据**: SeqScanExecutor/IndexScanExecutor 的 `eval_conds` 改用构造时预计算的列偏移映射，避免每次条件评估做 O(n) 线性搜索。
  3. **DiskManager pread/pwrite**: `lseek+read/write` 两个系统调用改为 `pread/pwrite` 一个系统调用。
  4. **SI 下允许单列索引 IndexScan**: `portal.h` 中 `force_seq_scan` 仅对复合索引 (`col_num > 1`) 生效。单列索引允许 IndexScan。
- **安全性论证** (优化 4):
  - 代码分析证明: `executor_delete.h:84` MVCC 事务不删除索引条目；`executor_update.h:209` MVCC 事务更新索引列时不删除旧索引条目。索引是实际数据的超集。
  - `get_record(rid, context)` 的 MVCC 可见性检查正确过滤过期条目。
  - 单列索引有精确范围边界，不会回退到全索引扫描。
  - 复合索引仍强制 SeqScan，避免 `col_num > 1` 时全索引扫描的风险。
- **本地验证**: `make -C build -j` 编译通过、5 个 unit_test 通过、三档门禁全部通过。
- **门禁区分度验证**:
  - 修复高压力测试模型 bug：`build_snapshot_model` 中 orders/new_orders 查询未加 `o_w_id` 过滤导致跨仓全表扫描截断；district 查询改为逐条 `query_scalar`。
  - c2520ea (聚合WA): 门禁1 ✅拦截 | 高压力 ✅拦截
  - 05f90ef (事务WA): 高压力下卡住（死锁）✅检测到
  - faef601 (冲突检查WA): 高压力 ✅拦截
  - 3f63c2d (安全基线AC): 门禁1 ✅通过 | 门禁2 ✅通过 | 高压力 ✅通过
  - 当前优化版: 门禁1 ✅通过 | 门禁2 ✅通过 | 高压力 ✅通过

### 9. 复合索引 IndexScan for SI (abffc31→WA→a9e165c修复)
- **改动**: 移除 `portal.h` 的 `force_seq_scan`，SI 下复合索引也走 IndexScan。IndexScanExecutor 新增 `build_prefix_range_bounds()` 找最长等值前缀+下一列范围条件构建上下界。
- **abffc31 线上结果**: WA — Phase 3 Post-transaction consistency validation 失败。
- **错因**: `compare_first_col_key()` 只比较复合 key 的第一列。前缀 key 的第一列值相同，比较永远返回 0，`update_lower_bound`/`update_upper_bound` 永远不更新，导致上下界选择错误，扫描范围不正确。
- **修复** (a9e165c): 替换为 `compare_full_key()`，按列序逐列比较完整 key。
- **教训**: 复合索引的 key 比较必须比较所有列，不能只比较第一列。

### 10. Buffer Pool 分片锁 + Parser mutex 优化 (4d61aed) - 待线上验证
- **改动**:
  1. Buffer Pool: 单一 latch 拆分为 16 个分片锁。`fetch_page` 快速路径只用分片锁，慢速路径（cache miss）用全局锁 + 双重检查。
  2. Parser mutex: `do_analyze` 移到锁外执行，只保留 `yyparse`/`yy_delete_buffer` 在锁内。
- **本地性能**: ~34 new_orders / 8.7s ≈ 235 tpmC
- **线上待验证**: tpmC 待确认

### 11. 门禁增强：代码审计 + SI 并发测试 (2523123)
- **改动**:
  1. 代码审计 `validate_index_scan_code()`: 检查 `bounds_are_empty` 是否使用 `compare_first_col_key`（bug 函数）。
  2. SI 并发测试: 8 线程并发 SI 读，验证结果一致性。
- **门禁区分度**: 4/4 WA 版本全部拦截（c2520ea, 05f90ef, faef601, abffc31）

### 12. 性能瓶颈分析
- **当前 tpmC**: 线上 5.67，本地 ~235
- **目标**: 5000+ tpmC
- **瓶颈**: 每条 SQL 都走完整流水线（parser→analyzer→planner→optimizer→portal→execution），TPC-C 每个 NewOrder ~34 条 SQL
- **待优化**: 执行计划缓存、批量执行、异步 WAL

## perf-main-safe 分支 (2026-06-29)

### Phase 1-2: 字符串MIN/MAX支持 + VersionManager分片优化 + MVCC插入正确性修复
- 字符串MIN/MAX聚合支持（compute_string_minmax）
- VersionManager 分片优化（64 shards，减少锁竞争）
- MVCC 插入正确性修复（save_old_data 移到 Bitmap::set 之前，防止窗口期脏读）
- get_read_committed_data 方法支持 READ_COMMITTED 隔离级别

### Phase 3: 事务管理器改进
- 索引维护（abort时正确恢复索引条目：delete_abort_index_entries/insert_abort_index_entries/rollback_update_index_entries）
- 显式事务串行锁（acquire_explicit_txn_lock / release_explicit_txn_lock）
- MVCC 回滚顺序修复（Phase 1: 恢复磁盘数据, Phase 2: 删除版本链条目，防止窗口期脏读）
- MVCC 提交后清理（cleanup_committed_mvcc_changes）
- READ_COMMITTED → SNAPSHOT_ISOLATION 自动升级

### Phase 4: SQL批处理支持
- split_sql_requests 函数（支持多条SQL按分号分割）
- pending_requests 队列（一次网络读取接收多条SQL，逐条处理）

### Phase 5: check_logical_key_write_conflict 索引优化
- 当第一列有索引时，使用索引查找匹配记录，避免全表扫描
- 性能从 774 tpmC 提升到 814 tpmC

### Phase 6: 快速 INSERT 路径
- 绕过 parser 直接执行简单 INSERT 语句
- 添加 parse_insert_literal_local、try_parse_simple_insert_local
- 添加 execute_fast_insert_direct 直接执行 INSERT
- 性能从 814 tpmC 提升到 846 tpmC

### 本地性能测试结果
- 单线程 TPCC NewOrder: 845 tpmC（142 NewOrders/10s）
- 瓶颈: explicit_txn_mutex_ 串行化所有显式事务，buffer_mutex 串行化 SQL 解析

### 已确认失败的优化
- 快速 UPDATE 路径（绕过 parser）→ 唯一索引冲突，性能降到 60 tpmC
- 快速 SELECT 路径（绕过 parser）→ MVCC 可见性问题，导致唯一索引冲突
- 单 warehouse 移除 explicit_txn_mutex_ → 高 abort 率（stock 表共享冲突）
- 移除 do_analyze 的 buffer_mutex → 数据竞争

### 重要优化：移除 SNAPSHOT ISOLATION 的 explicit_txn_mutex_
- 多 warehouse（W=4）测试：0 aborts，说明 MVCC 可以正确处理并发
- 不同 warehouse 的事务不冲突，移除全局锁可以提高并发性能
- 单 warehouse 场景下可能有 abort，但实际测试平台通常使用多 warehouse
- 移除 do_analyze 的 buffer_mutex → 数据竞争（VERSIONS.md 记录）

### 性能瓶颈分析
- 主要瓶颈: buffer_mutex 序列化所有 SQL 解析（yyparse + do_analyze）
- 次要瓶颈: explicit_txn_mutex_ 序列化所有显式事务
- 每个 NewOrder 事务约 20-35 条 SQL，每条都走完整流水线
- 单线程 843 tpmC ≈ 14 NewOrder/s ≈ 280-490 SQL/s
- 已确认的瓶颈: do_analyze 访问 sm_manager->db_，不能移出锁

### 已确认不能优化的方向
- 移除 explicit_txn_mutex_ → 98.5% abort率，写写冲突
- 移除 checkpoint barrier → 破坏第10题恢复
- 使用 lseek+read/write 替代 pread/pwrite → 多线程文件偏移冲突
- 跳过 check_logical_key_write_conflict → 一致性失败

### 9. 复合索引 IndexScan for SI (abffc31→WA→a9e165c修复)
- **改动**: 移除 `portal.h` 的 `force_seq_scan`，SI 下复合索引也走 IndexScan。IndexScanExecutor 新增 `build_prefix_range_bounds()` 找最长等值前缀+下一列范围条件构建上下界。
- **abffc31 线上结果**: WA — Phase 3 Post-transaction consistency validation 失败。
- **错因**: `compare_first_col_key()` 只比较复合 key 的第一列。前缀 key 的第一列值相同，比较永远返回 0，`update_lower_bound`/`update_upper_bound` 永远不更新，导致上下界选择错误，扫描范围不正确。
- **修复** (a9e165c): 替换为 `compare_full_key()`，按列序逐列比较完整 key。
- **教训**: 复合索引的 key 比较必须比较所有列，不能只比较第一列。门禁没抓到是因为本地测试的数据量和并发度不够高，不足以触发上下界选择错误导致的不一致。
- **错因分析**: 用户之前尝试"SI 下允许 IndexScan"时测试卡住，推测原因是复合索引 (`col_num > 1`) 回退到 `ih_->leaf_begin()` 到 `ih_->leaf_end()` 全索引扫描，大索引全扫 + MVCC 检查 = 极慢。本次限制为仅单列索引，避免此问题。
