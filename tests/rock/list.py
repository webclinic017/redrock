from conn import r, rock_evict


key = "_test_rock_list_"


def lpush():
    r.execute_command("del", key)
    e0 = "a"
    e1 = "1"
    e2 = "bbb"
    r.lpush(key, e2)
    rock_evict(key)
    r.lpush(key, e1)
    res = r.lpush(key, e0)
    if res != 3:
        raise "lpush fail"


def rpush():
    r.execute_command("del", key)
    e0 = "cd"
    e1 = "xxc"
    e2 = "3"
    r.rpush(key, e0)
    rock_evict(key)
    res = r.rpush(key, e1, e2)
    if res != 3:
        raise "rpush fail"


def lrange():
    r.execute_command("del", key)
    e1 = "dz"
    e2 = "2"
    e3 = "ei"
    r.lpush(key, e3, e2, e1)
    rock_evict(key)
    res = r.lrange(key, 1, -1)
    if res != [e2, e3]:
        raise "lrange fail"


def llen():
    r.execute_command("del", key)
    r.lpush(key, "a", "bb")
    rock_evict(key)
    res = r.llen(key)
    if res != 2:
        raise "llen fail"


def lindex():
    r.execute_command("del", key)
    e0 = "t"
    e1 = "ab"
    e2 = "xxx"
    e3 = "4"
    r.lpush(key, e3, e2, e1, e0)
    rock_evict(key)
    res = r.lindex(key, 1)
    if res != e1:
        raise "lindex fail"


def lpop():
    r.execute_command("del", key)
    r.rpush(key, "one", "two", "three", "four", "five")
    rock_evict(key)
    res = r.execute_command("lpop", key, 2)
    if res != ["one", "two"]:
        raise "lpop fail"


def rpop():
    r.execute_command("del", key)
    r.rpush(key, "one", "two", "three", "four", "five")
    rock_evict(key)
    res = r.lpop(key)
    if res != "one":
        raise "rpop fail"


def linsert():
    r.execute_command("del", key)
    r.rpush(key, "Hello", "World")
    rock_evict(key)
    r.linsert(key, "before", "World", "There")
    res = r.lrange(key, 0, -1)
    if res != ["Hello", "There", "World"]:
        raise "linsert fail"


def lrem():
    r.execute_command("del", key)
    r.rpush(key, "hello", "hello", "foo", "hello")
    rock_evict(key)
    r.lrem(key, -2, "hello")
    res = r.lrange(key, 0, -1)
    if res != ["hello", "foo"]:
        raise "lrem fail"


def rpoplpush():
    myotherlist = key + "_mother"
    r.execute_command("del", key)
    r.execute_command("del", myotherlist)
    r.rpush(key, "one", "two", "three")
    rock_evict(key)
    r.rpoplpush(key, myotherlist)
    res = r.lrange(key, 0, -1)
    if res != ["one", "two"]:
        raise "rpoplpush fail"
    res = r.lrange(myotherlist, 0, -1)
    if res != ["three"]:
        raise "rpoplpush fail2"


def lpushx():
    myotherlist = key + "_mother"
    r.execute_command("del", key)
    r.execute_command("del", myotherlist)
    r.lpush(key, "World")
    rock_evict(key)
    res = r.lpushx(key, "Hello")
    if res != 2:
        raise "lpushhx fail"
    res = r.lrange(key, 0, -1)
    if res != ["Hello", "World"]:
        raise "lpushhx fail2"
    rock_evict(myotherlist)
    res = r.lpushx(myotherlist, "Hello")
    if res != 0:
        raise "lpushhx fail3"
    res = r.lrange(myotherlist, 0, -1)
    if res != []:
        raise "lpushhx fail4"


def rpushx():
    myotherlist = key + "_mother"
    r.execute_command("del", key)
    r.execute_command("del", myotherlist)
    r.rpush(key, "Hello")
    rock_evict(key)
    res = r.rpushx(key, "World")
    if res != 2:
        raise "rpushx fail"
    rock_evict(myotherlist)
    res = r.rpushx(myotherlist, "World")
    if res != 0:
        raise "rpushx fail2"
    res = r.lrange(key, 0, -1)
    if res != ["Hello", "World"]:
        raise "rpushx fail3"
    res = r.lrange(myotherlist, 0, -1)
    if res != []:
        raise "rpushx fail4"


def lpos():
    r.execute_command("del", key)
    r.rpush(key, "a", "b", "c", "d", 1, 2, 3, 4, 3, 3, 3)
    rock_evict(key)
    res = r.execute_command("lpos", key, 3)
    if res != 6:
        raise "lpos fail"
    rock_evict(key)
    res = r.execute_command("lpos", key, 3, "count", 0, "rank", 2)
    if res != [8, 9, 10]:
        raise "lpos fail2"


def lset():
    r.execute_command("del", key)
    r.rpush(key, "one", "two", "three")
    rock_evict(key)
    r.execute_command("lset", key, 0, "four")
    rock_evict(key)
    r.execute_command("lset", key, -2, "five")
    res = r.lrange(key, 0, -1)
    if res != ["four", "five", "three"]:
        raise "lset fail"


def lmove():
    myotherlist = key + "_mother"
    r.execute_command("del", key)
    r.execute_command("del", myotherlist)
    r.rpush(key, "one", "two", "three")
    rock_evict(key)
    res = r.execute_command("lmove", key, myotherlist, "right", "left")
    if res != "three":
        raise "lmove fail"
    rock_evict(key)
    res = r.execute_command("lmove", key, myotherlist, "left", "right")
    if res != "one":
        raise "lmove fail2"
    res = r.lrange(key, 0, -1)
    if res != ["two"]:
        raise "lmove fail3"
    res = r.lrange(myotherlist, 0, -1)
    if res != ["three", "one"]:
        raise "lmove fail4"


def ltrim():
    r.execute_command("del", key)
    r.rpush(key, "one", "two", "three")
    rock_evict(key)
    r.execute_command("ltrim", key, 1, -1)
    res = r.lrange(key, 0, -1)
    if res != ["two", "three"]:
        raise "ltrim fail"


def _main():
    lpush()
    rpush()
    lpop()
    rpop()
    lrange()
    llen()
    lindex()
    linsert()
    lrem()
    rpoplpush()
    lpushx()
    rpushx()
    lpos()
    lset()
    lmove()
    ltrim()


if __name__ == '__main__':
    _main()