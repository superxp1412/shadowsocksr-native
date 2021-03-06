#include <stdio.h>
#include <assert.h>
#include <memory.h>

#if !defined(_WIN32)
#include <netdb.h>
#endif // !defined(_WIN32)

#include "sockaddr_universal.h"

bool socks5_address_parse(const uint8_t *data, size_t len, struct socks5_address *addr) {
    int offset     = 0;
    size_t addr_size = 0;
    uint8_t addr_type = 0;

    if (data==NULL || len==0 || addr==NULL) {
        return false;
    }

    addr_type = data[offset++];

    switch ((enum SOCKS5_ADDRTYPE)addr_type) {
        case SOCKS5_ADDRTYPE_IPV4:
            addr_size = sizeof(struct in_addr);
            if (len < sizeof(uint8_t) + addr_size + sizeof(uint16_t)) {
                return false;
            }
            addr->addr_type = SOCKS5_ADDRTYPE_IPV4;
            memcpy(&addr->addr.ipv4, data + offset, addr_size);
            break;
        case SOCKS5_ADDRTYPE_DOMAINNAME:
            addr_size = (size_t)data[offset++];
            if (len < sizeof(uint8_t) + sizeof(uint8_t) + addr_size + sizeof(uint16_t)) {
                return false;
            }
            addr->addr_type = SOCKS5_ADDRTYPE_DOMAINNAME;
            memset(addr->addr.domainname, 0, sizeof(addr->addr.domainname));
            memcpy(addr->addr.domainname, data+offset, addr_size);
            break;
        case SOCKS5_ADDRTYPE_IPV6:
            addr_size = sizeof(struct in6_addr);
            if (len < sizeof(uint8_t) + addr_size + sizeof(uint16_t)) {
                return false;
            }
            addr->addr_type = SOCKS5_ADDRTYPE_IPV6;
            memcpy(&addr->addr.ipv6, data + offset, addr_size);
            break;
        default:
            addr->addr_type = SOCKS5_ADDRTYPE_INVALID;
            return false;
            break;
    }
    offset += addr_size;

    addr->port = ntohs( *((uint16_t *)(data+offset)) );

    offset += sizeof(uint16_t);

    return true;
}

size_t socks5_address_size(const struct socks5_address *addr) {
    size_t size = 0;
    do {
        if (addr == NULL) {
            break;
        }
        switch (addr->addr_type) {
            case SOCKS5_ADDRTYPE_IPV4:
                size = sizeof(uint8_t) + sizeof(struct in_addr) + sizeof(uint16_t);
                break;
            case SOCKS5_ADDRTYPE_DOMAINNAME:
                size = sizeof(uint8_t) + sizeof(uint8_t) + strlen(addr->addr.domainname) + sizeof(uint16_t);
                break;
            case SOCKS5_ADDRTYPE_IPV6:
                size = sizeof(uint8_t) + sizeof(struct in6_addr) + sizeof(uint16_t);
                break;
            default:
                break;
        }
    } while (0);
    return size;
}

uint8_t * socks5_address_binary(const struct socks5_address *addr, uint8_t *buffer, size_t size) {
    int offset     = 0;
    size_t addr_size = 0;
    if (addr==NULL || buffer==NULL || size==0) {
        return NULL;
    }
    if (size < socks5_address_size(addr)) {
        return NULL;
    }

    buffer[offset++] = (uint8_t)addr->addr_type;

    switch (addr->addr_type) {
        case SOCKS5_ADDRTYPE_IPV4:
            memcpy(buffer+offset, &addr->addr.ipv4, sizeof(struct in_addr));
            offset += sizeof(struct in_addr);
            break;
        case SOCKS5_ADDRTYPE_DOMAINNAME:
            addr_size = strlen(addr->addr.domainname);
            buffer[offset++] = (uint8_t)addr_size;
            memcpy(buffer+offset, addr->addr.domainname, addr_size);
            offset += addr_size;
            break;
        case SOCKS5_ADDRTYPE_IPV6:
            memcpy(buffer+offset, &addr->addr.ipv6, sizeof(struct in6_addr));
            offset += sizeof(struct in6_addr);
            break;
        default:
            return NULL;
    }
    *((uint16_t *)(buffer + offset)) = htons(addr->port);
    return buffer;
}

int convert_address(const char *addr_str, unsigned short port, union sockaddr_universal *addr)
{
    struct addrinfo hints = { 0 }, *ai = NULL;
    int status;
    char port_buffer[6] = { 0 };
    int result = -1;

    if (addr_str == NULL || port == 0 || addr == NULL) {
        return result;
    }

    sprintf(port_buffer, "%hu", port);

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE;

    if ((status = getaddrinfo(addr_str, port_buffer, &hints, &ai)) != 0) {
        return result;
    }

    // Note, we're taking the first valid address, there may be more than one
    switch (ai->ai_family) {
    case AF_INET:
        addr->addr4 = *(const struct sockaddr_in *) ai->ai_addr;
        addr->addr4.sin_port = htons(port);
        result = 0;
        break;
    case AF_INET6:
        addr->addr6 = *(const struct sockaddr_in6 *) ai->ai_addr;
        addr->addr6.sin6_port = htons(port);
        result = 0;
        break;
    default:
        assert(0);
        break;
    }

    freeaddrinfo(ai);
    return result;
}
