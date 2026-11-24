/*
 * route - 경로 접두사로 백엔드를 고르는 규칙 표
 */
#include "route.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static const char *backend_parse(struct backend *b, const char *s, size_t len);

const char *route_table_add(struct route_table *table, const char *rule)
{
    struct route *route = &table->routes[table->count];
    const char *eq = strchr(rule, '=');
    const char *p;
    size_t prefix_len;

    if (table->count == ROUTE_MAX) {
        return "규칙이 너무 많습니다";
    }
    if (eq == NULL) {
        return "'접두사=백엔드' 형식이 아닙니다";
    }
    prefix_len = (size_t)(eq - rule);
    /* 끝의 '/'까지 비교해야 "/api/" 규칙이 "/apix/..."에 걸리지 않음 */
    if (prefix_len == 0 || rule[0] != '/' || eq[-1] != '/') {
        return "접두사는 '/'로 시작하고 '/'로 끝나야 합니다";
    }
    if (prefix_len >= ROUTE_PREFIX_MAX) {
        return "접두사가 너무 깁니다";
    }
    memset(route, 0, sizeof(*route));
    memcpy(route->prefix, rule, prefix_len);
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->routes[i].prefix, route->prefix) == 0) {
            return "같은 접두사가 두 번 나왔습니다";
        }
    }

    /* 쉼표로 나뉜 백엔드 목록 */
    p = eq + 1;
    while (1) {
        size_t len = strcspn(p, ",");
        const char *error;
        if (route->backend_count == ROUTE_BACKEND_MAX) {
            return "백엔드가 너무 많습니다";
        }
        error = backend_parse(&route->backends[route->backend_count], p, len);
        if (error != NULL) {
            return error;
        }
        route->backend_count++;
        if (p[len] == '\0') {
            break;
        }
        p += len + 1;
    }

    if (pthread_mutex_init(&route->lock, NULL) != 0) {
        return "규칙의 락을 만들지 못했습니다";
    }
    table->count++;
    return NULL;
}

/* "이름:포트"를 주소로 바꿉니다. IPv4만 씁니다 (서버도 IPv4로만 기다림) */
static const char *backend_parse(struct backend *b, const char *s, size_t len)
{
    struct addrinfo hints, *info;
    char host[BACKEND_NAME_MAX];
    const char *port;
    char *colon, *end;
    long number;

    if (len >= sizeof(host)) {
        return "백엔드 이름이 너무 깁니다";
    }
    memcpy(host, s, len);
    host[len] = '\0';
    memcpy(b->name, host, len + 1);

    colon = strrchr(host, ':');
    if (colon == NULL || colon == host) {
        return "백엔드는 '이름:포트' 형식이어야 합니다";
    }
    *colon = '\0';
    port = colon + 1;
    number = strtol(port, &end, 10);
    if (*port < '0' || *port > '9' || *end != '\0' || number < 1 ||
        number > 65535) {
        return "백엔드 포트는 1~65535 숫자여야 합니다";
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &info) != 0) {
        return "백엔드 이름을 주소로 바꾸지 못했습니다";
    }
    memcpy(&b->addr, info->ai_addr, sizeof(b->addr));
    freeaddrinfo(info);
    return NULL;
}

void route_table_destroy(struct route_table *table)
{
    for (int i = 0; i < table->count; i++) {
        pthread_mutex_destroy(&table->routes[i].lock);
    }
    table->count = 0;
}

struct route *route_table_find(struct route_table *table, const char *path,
                               size_t len)
{
    struct route *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < table->count; i++) {
        struct route *route = &table->routes[i];
        size_t n = strlen(route->prefix);
        if (n <= len && n > best_len && memcmp(path, route->prefix, n) == 0) {
            best = route;
            best_len = n;
        }
    }
    return best;
}

int route_next_backend(struct route *route)
{
    int index;

    pthread_mutex_lock(&route->lock);
    index = route->next;
    route->next = (route->next + 1) % route->backend_count;
    pthread_mutex_unlock(&route->lock);
    return index;
}
