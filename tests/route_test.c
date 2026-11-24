/*
 * route 단위 테스트: 규칙 해석, 가장 긴 접두사, Round-Robin
 */
#include "acutest.h"
#include "route.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct route_table *new_table(void)
{
    return calloc(1, sizeof(struct route_table));
}

static void free_table(struct route_table *t)
{
    route_table_destroy(t);
    free(t);
}

/* 경로에 걸린 규칙의 접두사. 없으면 "" */
static const char *found(struct route_table *t, const char *path)
{
    struct route *r = route_table_find(t, path, strlen(path));
    return r ? r->prefix : "";
}

static void test_rule_is_parsed(void)
{
    struct route_table *t = new_table();

    TEST_CHECK(route_table_add(t, "/api/=127.0.0.1:8001,127.0.0.1:8002") ==
               NULL);
    TEST_CHECK(t->count == 1);
    TEST_CHECK(strcmp(t->routes[0].prefix, "/api/") == 0);
    TEST_CHECK(t->routes[0].backend_count == 2);
    TEST_CHECK(strcmp(t->routes[0].backends[1].name, "127.0.0.1:8002") == 0);
    TEST_CHECK(ntohs(t->routes[0].backends[1].addr.sin_port) == 8002);
    free_table(t);
}

static void test_bad_rules_are_rejected(void)
{
    static const char *const bad[] = {
        "/api/",
        "=127.0.0.1:8001",
        "api/=127.0.0.1:8001",
        "/api=127.0.0.1:8001",
        "/api/=",
        "/api/=127.0.0.1",
        "/api/=:8001",
        "/api/=127.0.0.1:",
        "/api/=127.0.0.1:0",
        "/api/=127.0.0.1:65536",
        "/api/=127.0.0.1:80x",
        "/api/=127.0.0.1:-1",
        "/api/=127.0.0.1:8001,",
        "/api/=,127.0.0.1:8001",
    };
    struct route_table *t = new_table();

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_CHECK_(route_table_add(t, bad[i]) != NULL, "%s", bad[i]);
    }
    TEST_CHECK(t->count == 0);
    free_table(t);
}

static void test_duplicate_prefix_is_rejected(void)
{
    struct route_table *t = new_table();

    TEST_CHECK(route_table_add(t, "/api/=127.0.0.1:8001") == NULL);
    TEST_CHECK(route_table_add(t, "/api/=127.0.0.1:8002") != NULL);
    TEST_CHECK(t->count == 1);
    free_table(t);
}

static void test_too_many_rules_and_backends(void)
{
    struct route_table *t = new_table();
    char rule[256] = "/many/=127.0.0.1:1";

    for (int i = 1; i <= ROUTE_BACKEND_MAX; i++) {
        strcat(rule, ",127.0.0.1:1");
    }
    TEST_CHECK(route_table_add(t, rule) != NULL);
    for (int i = 0; i < ROUTE_MAX; i++) {
        snprintf(rule, sizeof(rule), "/r%d/=127.0.0.1:1", i);
        TEST_CHECK(route_table_add(t, rule) == NULL);
    }
    TEST_CHECK(route_table_add(t, "/last/=127.0.0.1:1") != NULL);
    free_table(t);
}

static void test_longest_prefix_wins(void)
{
    struct route_table *t = new_table();

    TEST_CHECK(route_table_add(t, "/api/=127.0.0.1:1") == NULL);
    TEST_CHECK(route_table_add(t, "/api/v2/=127.0.0.1:2") == NULL);
    TEST_CHECK(strcmp(found(t, "/api/v2/users"), "/api/v2/") == 0);
    TEST_CHECK(strcmp(found(t, "/api/v1/users"), "/api/") == 0);
    TEST_CHECK(strcmp(found(t, "/api/"), "/api/") == 0);
    free_table(t);
}

static void test_prefix_matches_whole_segments(void)
{
    struct route_table *t = new_table();

    TEST_CHECK(route_table_add(t, "/api/=127.0.0.1:1") == NULL);
    TEST_CHECK(strcmp(found(t, "/apix/health"), "") == 0);
    TEST_CHECK(strcmp(found(t, "/api"), "") == 0);
    TEST_CHECK(strcmp(found(t, "/home.html"), "") == 0);
    TEST_CHECK(strcmp(found(t, "/x/api/"), "") == 0);
    /* 길이를 넘어선 글자(쿼리)는 보지 않음 */
    TEST_CHECK(route_table_find(t, "/api/", 4) == NULL);
    free_table(t);
}

static void test_self_backend_is_found(void)
{
    static const char *const self[] = {"/a/=127.0.0.1:8000",
                                       "/a/=localhost:8000", "/a/=0.0.0.0:8000",
                                       "/a/=127.0.0.1:8001,127.0.0.1:8000"};
    static const char *const other[] = {"/a/=127.0.0.1:8001",
                                        "/a/=192.0.2.1:8000"};

    for (size_t i = 0; i < sizeof(self) / sizeof(self[0]); i++) {
        struct route_table *t = new_table();
        TEST_CHECK(route_table_add(t, self[i]) == NULL);
        TEST_CHECK_(route_table_find_self(t, 8000) != NULL, "%s", self[i]);
        free_table(t);
    }
    for (size_t i = 0; i < sizeof(other) / sizeof(other[0]); i++) {
        struct route_table *t = new_table();
        TEST_CHECK(route_table_add(t, other[i]) == NULL);
        TEST_CHECK_(route_table_find_self(t, 8000) == NULL, "%s", other[i]);
        free_table(t);
    }
}

static void test_round_robin(void)
{
    struct route_table *t = new_table();
    struct route *r;

    TEST_CHECK(route_table_add(t, "/a/=127.0.0.1:1,127.0.0.1:2,127.0.0.1:3") ==
               NULL);
    r = &t->routes[0];
    for (int i = 0; i < 7; i++) {
        TEST_CHECK(route_next_backend(r) == i % 3);
    }
    free_table(t);
}

#define RR_THREADS 4
#define RR_ROUNDS 3000

static void *take_turns(void *arg)
{
    struct route *r = arg;
    long *counts = calloc(ROUTE_BACKEND_MAX, sizeof(long));

    for (int i = 0; i < RR_ROUNDS; i++) {
        counts[route_next_backend(r)]++;
    }
    return counts;
}

static void test_round_robin_with_threads(void)
{
    struct route_table *t = new_table();
    pthread_t threads[RR_THREADS];
    long total[3] = {0};

    TEST_CHECK(route_table_add(t, "/a/=127.0.0.1:1,127.0.0.1:2,127.0.0.1:3") ==
               NULL);
    for (int i = 0; i < RR_THREADS; i++) {
        pthread_create(&threads[i], NULL, take_turns, &t->routes[0]);
    }
    for (int i = 0; i < RR_THREADS; i++) {
        long *counts;
        pthread_join(threads[i], (void **)&counts);
        for (int j = 0; j < 3; j++) {
            total[j] += counts[j];
        }
        free(counts);
    }
    for (int j = 0; j < 3; j++) {
        TEST_CHECK(total[j] == RR_THREADS * RR_ROUNDS / 3);
    }
    free_table(t);
}

TEST_LIST = {
    {"규칙을 접두사와 백엔드 목록으로 나눈다", test_rule_is_parsed},
    {"형식이 틀린 규칙은 거절한다", test_bad_rules_are_rejected},
    {"같은 접두사가 두 번이면 거절한다", test_duplicate_prefix_is_rejected},
    {"규칙과 백엔드가 상한을 넘으면 거절한다",
     test_too_many_rules_and_backends},
    {"여러 규칙이 맞으면 가장 긴 접두사를 고른다", test_longest_prefix_wins},
    {"/api/ 규칙은 /apix/에 걸리지 않는다", test_prefix_matches_whole_segments},
    {"이 서버 자신을 가리키는 백엔드를 찾는다", test_self_backend_is_found},
    {"요청마다 다음 백엔드를 차례로 고른다", test_round_robin},
    {"여러 스레드가 골라도 백엔드마다 같은 횟수다",
     test_round_robin_with_threads},
    {NULL, NULL}};
