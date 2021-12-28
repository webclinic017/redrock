/* Here I save some code which is not used but maybe referennce
 */

/* rock_reead.c */

static inline int is_rock_key_has_dbid(const int dbid, const sds rock_key)
{
    const int dbid_in_rock_key = rock_key[0];
    return dbid == dbid_in_rock_key ? 1 : 0;
}

static inline int is_rock_key_has_dbid(const int dbid, const sds rock_key)
{
    const int dbid_in_rock_key = rock_key[0];
    return dbid == dbid_in_rock_key ? 1 : 0;
}


/* Called in main thread when delete a whole db.
 * Reference delete_key() but this implementation is quicker
 */
static void delete_whole_db(const int dbid, list *client_ids)
{
    rock_r_lock();
    // first deal with read_key_tasks
    for (int i = 0; i < READ_TOTAL_LEN; ++i)
    {
        sds task = read_key_tasks[i];
        if (task == NULL)
            break;

        if (task == outdate_key_flag)
            continue;

        if (is_rock_key_has_dbid(dbid, task))
        {
            read_key_tasks[i] = outdate_key_flag;
            if (read_return_vals[i] != NULL)
                sdsfree(read_return_vals[i]);
        }        
    }

    // second deal with read_rock_key_candidates
    list *will_deleted = listCreate();
    dictIterator *di = dictGetIterator(read_rock_key_candidates);
    dictEntry *de;
    while ((de = dictNext(di)))
    {
        sds rock_key = dictGetKey(de);
        if (is_rock_key_has_dbid(dbid, rock_key))
        {
            listAddNodeTail(will_deleted, rock_key);
            list *waiting = dictGetVal(de);
            dictGetVal(de) = NULL;
            listJoin(client_ids, waiting);
            serverAssert(listLength(waiting) == 0);
            listRelease(waiting);
        }
    }
    dictReleaseIterator(di);
    // delete will_deleted from read_rock_key_candidates
    listIter li;
    listNode *ln;
    listRewind(will_deleted, &li);
    while ((ln = listNext(&li)))
    {
        sds delete_rock_key = listNodeValue(ln);
        int res = dictDelete(read_rock_key_candidates, delete_rock_key);
        serverAssert(res == DICT_OK);
    }
    listRelease(will_deleted);
    rock_r_unlock();
}

/* Called in main thread
 * The caller guarantee that all keys in db of dbid must be deleted first.
 */
void on_delete_whole_db_in_command(const int dbid)
{
    list *client_ids = listCreate();
    delete_whole_db(dbid, client_ids);
    clients_check_resume_for_rock_key_update(client_ids);
    listRelease(client_ids);
}

/* Called in main thread.
 * The caller guarantee that the keys must be deleted from redis db first
 * then call into the on_delete_keys_in_command().
 * Such command like DEL, visit expired key, MIGRATE, MOVE, UNLINK
 * So the resume commands can guarantee the atomic in order.
 */
void on_delete_keys_in_command(const int dbid, const list* redis_keys)
{
    list *client_ids = listCreate();
    listIter li;
    listNode *ln;
    listRewind((list*)redis_keys, &li);
    while ((ln = listNext(&li)))
    {
        const sds redis_key = listNodeValue(ln);
        delete_key(dbid, redis_key, client_ids);
    }
    clients_check_resume_for_rock_key_update(client_ids);
    listRelease(client_ids);
}

void debug_add_tasks(const int cnt, const int* const dbids, const sds* keys)
{
    rock_r_lock();

    for (int i = 0; i < cnt; ++i)
    {
        sds copy = sdsdup(keys[i]);
        copy = encode_rock_key(dbids[i], copy);

        read_key_tasks[i] = copy;
        read_return_vals[i] = NULL;
    }

    task_status = READ_START_TASK;

    rock_r_unlock();
}

/* This is called in main thread and as the API for rock_write.c
 * when it will write some rock keys to RocksDB
 * We will delete all rock_keys in read_rock_key_candidates
 * with setting outdate_key_flag in read_key_tasks 
 * before they are writtent to RocksDB.
 */
void cancel_read_task_before_write_to_rocksdb(const int len, const sds *rock_keys)
{
    rock_r_lock();

    for (int i = 0; i < len; ++i)
    {
        dictEntry *de = dictFind(read_rock_key_candidates, rock_keys[i]);
        if (de)
        {
            const sds rock_key_in_candidates = dictGetKey(de);
            set_outdate_flag(rock_key_in_candidates);
            // After set outdate flag, we can delete the candidate.
            // We do not need to resume the clients because 
            // the API caller guarantee these keys change to rock val.
            int res = dictDelete(read_rock_key_candidates, rock_key_in_candidates);
            serverAssert(res == DICT_OK);
        }
    }

    rock_r_unlock();
}

/* This is called in main thread to set outdate flag
 * It guarantee in lock mode by the caller.
 * with freeing return val if needed.
 */
static void set_outdate_flag(const sds rock_key_in_candidates)
{
    for (int i = 0; i < READ_TOTAL_LEN; ++i)
    {
        sds task = read_key_tasks[i];
        if (task == NULL)
            break;

        // Because read_key_tasks share the same key with candidates
        // We can use address comparison
        if (task == rock_key_in_candidates)
        {
            read_key_tasks[i] = outdate_key_flag;
            if (read_return_vals[i])
                sdsfree(read_return_vals[i]);
            break;
        }
    }
}

/* Work in read thead to read the needed real keys, i.e., rocksdb_keys.
 * The caller guarantees not in lock mode.
 * NOTE: no need to work in lock mode because keys is duplicated from read_key_tasks
 */
static int read_from_rocksdb(const int cnt, const sds* keys, sds* vals)
{
    char* rockdb_keys[READ_TOTAL_LEN];
    size_t rockdb_key_sizes[READ_TOTAL_LEN];
    char* rockdb_vals[READ_TOTAL_LEN];
    size_t rockdb_val_sizes[READ_TOTAL_LEN];
    char* errs[READ_TOTAL_LEN];

    int real_read_cnt = 0;
    for (int i = 0; i < cnt; ++i)
    {
        serverAssert(keys[i]);
        if (keys[i] == outdate_key_flag)
            continue;
        
        rockdb_keys[real_read_cnt] = keys[i];
        rockdb_key_sizes[real_read_cnt] = sdslen(keys[i]);
        ++real_read_cnt;
    }

    if (real_read_cnt == 0)
        return 0;   // all input keys are outdate_key_flag

    rocksdb_readoptions_t *readoptions = rocksdb_readoptions_create();
    rocksdb_multi_get(rockdb, readoptions, real_read_cnt, 
                      (const char* const *)rockdb_keys, rockdb_key_sizes, 
                      rockdb_vals, rockdb_val_sizes, errs);
    rocksdb_readoptions_destroy(readoptions);

    int index_input = 0;
    int index_rock = 0;
    while (index_input < cnt)
    {
        if (keys[index_input] == outdate_key_flag)
        {
            ++index_input;
            continue;       // skip outdate_key_flag
        }
        
        if (errs[index_rock]) 
        {
            serverLog(LL_WARNING, "read_from_rocksdb() reading from RocksDB failed, err = %s, key = %s",
                      errs[index_rock], rockdb_keys[index_rock]);
            exit(1);
        }

        if (rockdb_vals[index_rock] == NULL)
        {
            // not found in RocksDB
            vals[index_input] = NULL;
        }
        else
        {
            // I think this code can not reroder in the caller's lock scope 
            vals[index_input] = sdsnewlen(rockdb_vals[index_rock], rockdb_val_sizes[index_rock]);
            // free the malloc memory from RocksDB API
            zlibc_free(rockdb_vals[index_rock]);  
        }

        ++index_input;
        ++index_rock;
    }    

    return real_read_cnt;
}

/* This is called by proocessInputBuffer() in netwroking.c
 * It will check the rock keys for current command in buffer
 * and if OK process the command and return.
 * Return C_ERR if processCommandAndResetClient() return C_ERR
 * indicating the caller need return to avoid looping and trimming the client buffer.
 * Otherwise, return C_OK, indicating in the caller, it can continue in the loop.
 */
int processCommandAndResetClient(client *c);        // networkng.c, no declaration in any header
int process_cmd_in_processInputBuffer(client *c)
{
    int ret = C_OK;

    list *rock_keys = get_keys_in_rock_for_command(c);
    if (rock_keys == NULL)
    {
        // NO rock_key or TRANSACTION with no EXEC command
        if (processCommandAndResetClient(c) == C_ERR)
            ret = C_ERR;
    }
    else
    {
        const int sync_mode = on_client_need_rock_keys(c, rock_keys);
        if (sync_mode)
        {
            if (processCommandAndResetClient(c) == C_ERR)
                ret = C_ERR;
        }
        listRelease(rock_keys);
    }

    return ret;
}

/* Called in main thread to recover one key, i.e., rock_key.
 * The caller guarantees lock mode, 
 * so be careful of no reentry of the lock.
 * Join (by moving to append) the waiting list for curent key to waiting_clients,
 * and delete the key from read_rock_key_candidates 
 * without destroy the waiting list for current rock_key.
 */
static void recover_one_key(const sds rock_key, const sds recover_val,
                            list *waiting_clients)
{
    int dbid;
    char *redis_key;
    size_t redis_key_len;
    decode_rock_key(rock_key, &dbid, &redis_key, &redis_key_len);
    try_recover_val_object_in_redis_db(dbid, redis_key, redis_key_len, recover_val);

    dictEntry *de = dictFind(read_rock_key_candidates, rock_key);
    serverAssert(de);

    list *current = dictGetVal(de);
    serverAssert(current);

    dictGetVal(de) = NULL;      // avoid clear the list of client ids in read_rock_key_candidates
    // task resource will be reclaimed (the list is NULL right now)
    dictDelete(read_rock_key_candidates, rock_key);

    listJoin(waiting_clients, current);
    serverAssert(listLength(current) == 0);
    listRelease(current);
}


static void debug_check_sds_equal(const int dbid, const sds redis_key, const robj *o, const sds field)
{
    redisDb *db = server.db + dbid;
    dictEntry *de_db = dictFind(db->dict, redis_key);
    serverAssert(de_db);
    serverAssert(dictGetKey(de_db) == redis_key);
    serverAssert(dictGetVal(de_db) == o);

    dict *hash = o->ptr;
    dictEntry *de_hash = dictFind(hash, field);
    serverAssert(de_hash);
    serverAssert(dictGetKey(de_hash) == field);
}

void debug_rock(client *c)
{
    sds flag = c->argv[1]->ptr;

    if (strcasecmp(flag, "evictkeys") == 0 && c->argc >= 3)
    {
        sds keys[RING_BUFFER_LEN];
        int dbids[RING_BUFFER_LEN];
        int len = 0;
        for (int i = 0; i < c->argc-2; ++i)
        {
            sds input_key = c->argv[i+2]->ptr;
            dictEntry *de = dictFind(c->db->dict, input_key);
            if (de)
            {
                if (!is_rock_value(dictGetVal(de)))
                {
                    serverLog(LL_NOTICE, "debug evictkeys, try key = %s", input_key);
                    keys[len] = input_key;
                    dbids[len] = c->db->id;
                    ++len;
                }
            }
        }
        if (len)
        {
            int ecvict_num = try_evict_to_rocksdb_for_db(len, dbids, keys);
            serverLog(LL_NOTICE, "debug evictkeys, ecvict_num = %d", ecvict_num);
        }
    }
    else if (strcasecmp(flag, "recoverkeys") == 0 && c->argc >= 3)
    {
        serverAssert(c->rock_key_num == 0);
        list *rock_keys = listCreate();
        for (int i = 0; i < c->argc-2; ++i)
        {
            sds input_key = c->argv[i+2]->ptr;
            dictEntry *de = dictFind(c->db->dict, input_key);
            if (de)
            {
                if (is_rock_value(dictGetVal(de)))
                {
                    serverLog(LL_WARNING, "debug_rock() found rock key = %s", (sds)dictGetKey(de));
                    listAddNodeTail(rock_keys, dictGetKey(de));
                }
            }
        }
        if (listLength(rock_keys) != 0)
        {
            on_client_need_rock_keys_for_db(c, rock_keys);
        }
        listRelease(rock_keys);
    }
    else if (strcasecmp(flag, "testread") == 0)
    {
        /*
        sds keys[2];
        keys[0] = sdsnew("key1");
        keys[1] = sdsnew("key2");
        sds copy_keys[2];
        copy_keys[0] = sdsdup(keys[0]);
        copy_keys[1] = sdsdup(keys[1]);
        robj* objs[2];
        char* val1 = "val_for_key1";
        char* val2 = "val_for_key2";       
        objs[0] = createStringObject(val1, strlen(val1));
        objs[1] = createStringObject(val2, strlen(val2));
        int dbids[2];
        dbids[0] = 65;      // like letter 'a'
        dbids[1] = 66;      // like letter 'b'
        write_batch_append_and_abandon(2, dbids, keys, objs);
        sleep(1);       // waiting for save to RocksdB
        debug_add_tasks(2, dbids, copy_keys);
        on_delete_key(dbids[0], copy_keys[1]);
        */
    }
    else if (strcasecmp(flag, "testwrite") == 0) 
    {
        /*
        int dbid = 1;

        int val_len = random() % 1024;
        sds val = sdsempty();
        for (int i = 0; i < val_len; ++i)
        {
            val = sdscat(val, "v");
        }

        sds keys[RING_BUFFER_LEN];
        robj* objs[RING_BUFFER_LEN];
        int dbids[RING_BUFFER_LEN];
        int cnt = 0;
        while (cnt < 5000000)
        {
            int space = space_in_write_ring_buffer();
            if (space == 0)
            {
                serverLog(LL_NOTICE, "space = 0, sleep for a while, cnt = %d", cnt);
                usleep(10000);
                continue;
            }
            
            int random_pick = random() % RING_BUFFER_LEN;
            if (random_pick == 0)
                random_pick = 1;
            
            const int pick = random_pick < space ? random_pick : space;

            for (int i = 0; i < pick; ++i)
            {
                sds key = debug_random_sds(128);
                sds val = debug_random_sds(1024);

                keys[i] = key;
                robj* o = createStringObject(val, sdslen(val));
                sdsfree(val);
                objs[i] = o;
                dbids[i] = dbid;
            }         
            write_batch_append_and_abandon(pick, dbids, keys, objs); 
            cnt += pick;
            serverLog(LL_NOTICE, "write_batch_append total = %d, cnt = %d", space, cnt);
        }
        */
    }
    else
    {
        addReplyError(c, "wrong flag for debugrock!");
        return;
    }

    addReplyBulk(c,c->argv[0]);
}




static void debug_print_lrus(const dict *lrus)
{
    dictIterator *di = dictGetIterator((dict*)lrus);
    dictEntry *de;
    while ((de = dictNext(di)))
    {
        const sds field = dictGetKey(de);
        serverLog(LL_NOTICE, "debug_print_lrus, field = %s", field);
    }

    dictReleaseIterator(di);
}

/* Called in main thread to set invalid flag for ring buffer.
 * The caller guarantee in lock mode.
 */
static void invalid_ring_buf_for_dbid(const int dbid)
{
    int index = rbuf_s_index;
    for (int i = 0; i < rbuf_len; ++i)
    {
        const sds rock_key = rbuf_keys[index];
        const int dbid_in_key = (unsigned char)rock_key[1];
        /*
        if (dbid_in_key == dbid)
            rbuf_invalids[index] = 1;
        */

        ++index;
        if (index == RING_BUFFER_LEN)
            index = 0;
    }
}

/* Called in main thread for flushdb or flushall( dbnum == -1) command
 * We need to set rbuf_invalids to true for these dbs,
 * because ring buffer can not removed from the middle,
 * but the rock read will lookup them from ring buufer by API 
 * get_vals_from_write_ring_buf_first_for_db() and get_vals_from_write_ring_buf_first_for_hash()
 */
void on_empty_db_for_rock_write(const int dbnum)
{
    return;     // do nothing

    int start = dbnum;
    if (dbnum == -1)
        start = 0;

    int end = dbnum + 1;
    if (dbnum == -1)
        end = server.dbnum;

    rock_w_lock();

    if (rbuf_len == 0)
    {
        rock_w_unlock();
        return;
    }

    for (int dbid = start; dbid < end; ++dbid)
    {
        invalid_ring_buf_for_dbid(dbid);
    }

    rock_w_unlock();
}