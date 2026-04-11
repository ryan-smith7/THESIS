#ifndef HTTP_GET_H
#define HTTP_GET_H

#include <zephyr/net/socket.h>

void nslookup(const char * hostname, struct zsock_addrinfo **results);

void print_addrinfo_results(struct zsock_addrinfo **results);

void http_get(int sock, char * hostname, char * url);

int connect_socket(struct zsock_addrinfo **results, uint16_t port);

int http_query(char * hostname, char * url);

#endif // HTTP_GET_H
