@@
     hyper_memory_free(plan_hmem);
     mdb_txn_abort(plan_txn);
 
     // --- Ждём материализации асинхронного вывода ---
-    bool found = false;
-    // Увеличиваем потолок ожидания до ~10s (200 * 50ms) и добавляем логирование попыток.
-    for (int attempt = 0; attempt < 200 && !found; attempt++) {
-        // Пауза 50ms: даём фоновому воркеру и db_writer больше времени
-        // для выполнения, но при этом не блокируем тест слишком долго.
-        usleep(50000); // 50ms
-        if ((attempt % 20) == 0) { // логируем каждые ~1s
-            fprintf(stderr, "[TEST] polling for derived atom, attempt=%d\n", attempt);
-        }
-
-        MDB_txn *poll_txn;
-        if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &poll_txn) != MDB_SUCCESS) continue;
-
-        HyperMemory poll_hm = {0};
-        poll_hm.txn             = poll_txn;
-        poll_hm.dbi_atoms       = db.graph.hyper.atoms;
-        poll_hm.dbi_idx_process = db.graph.hyper.idx_process;
-        poll_hm.dbi_idx_args    = db.graph.hyper.idx_args;
-        poll_hm.dbi_idx_context = db.graph.hyper.idx_context;
-
-        NeuroAtom *results = NULL;
-        size_t count = 0;
-
-        if (hyper_find_by_participant(&poll_hm, s.db_query, 0, &results, &count) == 0) {
-            for (size_t i = 0; i < count; i++) {
-                if (results[i].process_id == s.has_vuln_proc &&
-                    HYPER_GET_ID(results[i].args[0].raw) == s.db_query &&
-                    HYPER_GET_ID(results[i].args[1].raw) == s.sql_injection) {
-                    found = true;
-                    // ИСПРАВЛЕНИЕ 2: Убрали двойной free и двойной abort.
-                    // Просто выходим из цикла, все ресурсы штатно очистятся ниже.
-                    break;
-                }
-            }
-        }
-
-        if (results) free(results);
-        mdb_txn_abort(poll_txn);
-    }
-
-    assert(found && "SQL Injection vulnerability was not derived asynchronously in time");
+    // Используем общий helper, ждём до 10s
+    int found = wait_for_atom_with_args(s.db_query, s.has_vuln_proc, s.db_query, s.sql_injection, 10000);
+    assert(found && "SQL Injection vulnerability was not derived asynchronously in time");
***
