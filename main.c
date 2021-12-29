#include "time.h"
#include "stdbool.h"
#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <ctype.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <memory.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <sys/types.h>
#include <netdb.h>
#include <regex.h>
#include <pthread.h>


static const int MAX_QUEUE_CLIENTS_NUMBER = 10;

static const int SOCKET_CALL_ERROR = -1;
static const int BIND_ERROR = -1;
static const int ACCEPT_ERROR = -1;
static const int CONNECT_HOST_ERROR = -1;
static const int SETSOCKOPT_ERROR = -1;

static const int LISTENING_PORT = 8000;

static const int PTHREAD_CREATE_SUCCESS = 0;
static const int CLIENT_POLL_INDEX = 0;
static const int REMOTE_HOST_POLL_INDEX = 1;

static bool STOPPED_PROXY_SERVER = false;

pthread_mutex_t cacheMutex;
pthread_mutex_t clientsNumberMutex;

static const int CACHE_SIZE = 200;
static const int P = 5;    // prime number for the calculating hash

static int clientsNumber = 0;
static const int MAX_CLIENTS_NUMBER = 200;

static const int UNDEFINED_INDEX = -1;

static const int NO_FREE_PLACE = -1;

static const int EXTRA_LOTS_NUMBER = 50;
static const int ACCEPT_DESC_INDEX = 0;

static const int BUFFER_SIZE = 1024;

static const int IS_TERMINATED = -1;

static const int FULL_REQUEST_MAX_LENGTH = 1024;

static const char *SUPPORTED_SERVICE = "http";
static const char *HTTP_SUPPORTED_VERSION = "1.0";

static const char *URL_REGEX = "^(https?:\\/\\/)?([0-9a-zA-Z\\.-]+)([0-9\\/a-zA-Z\\.-_-]*)*$";

static const int REGEX_MATCHES_NUMBER = 4;
static const int PARSE_URL_SUCCESS = 0;
static const int PARSE_URL_FAILURE = -1;

static const int END_OF_DATA = 0;
static const short NO_EVENT = 0;
static const short NO_REVENT = 0;

typedef struct CacheRecord {
    char *URL;
    char *response;
} CacheRecord;

typedef struct Cache {
    CacheRecord **records;               // массив размера cacheSize
    unsigned int freeRecordsNumber;      //кол-во свободных мест (для поиска самой старой записи при надобности замещения: если == 0 и у нас коллизия, то сразу же ищем старую запись)
} Cache;

typedef enum matchesTypes {
    SCHEME = 1,
    HOST = 2,
    PATH = 3
} matchesTypes;

typedef struct structuralUrl {
    char *scheme;
    char *host;
    char *path;
} structuralUrl;

typedef struct ClientInfo {
    int clientSocket;
    int remoteHostSocket;
    bool isWaitingForResponse;
    bool isRequestSent;
    int cacheRecordIndex;
    bool isResponseReceived;
    structuralUrl *URL;
    char *response;
} ClientInfo;

typedef struct Client {
    ClientInfo *info;
    Cache **cache;
} Client;

bool isTheErrorSocketResult(int desc) {
    return desc == SOCKET_CALL_ERROR;
}

int isTheErrorAcceptResult(int desc) {
    return desc == ACCEPT_ERROR;
}

bool isIOErrorOccurred(const int returnedNumber) {
    return 0 > returnedNumber;
}

bool isErrorOccurred(const int errorNumber) {
    return 0 != errorNumber;
}

bool isTimeoutReached(const int pollStatus) {
    return 0 == pollStatus;
}

bool hasClientSentData(const short event) {
    return POLL_IN & event;
}

bool isWorkWithRemoteHostInProcess(const bool isRemoteSocketOpened) {
    return isRemoteSocketOpened == true;
}

bool hasHttpRequestBeenSent(const bool hasRequestSent) {
    return hasRequestSent == true;
}

bool isNewConnectionRequest(const short event) {
    return POLLIN & event;
}

bool hasRemoteHostSentResponse(const short event) {
    return POLLIN & event;
}

bool isHostReadyToReceiveRequest(const short event) {
    return (POLL_OUT & event) | (POLLWRBAND & event) | (POLLWRNORM & event);
}

bool isThereFreePlaceForAcceptingClient(int trackedDescNumber, int totalClientsNumber) {
    return (trackedDescNumber - 1) / 2 < totalClientsNumber;
}

bool isSocketDescUnavailable(const short event) {
    return POLL_ERR & event || POLL_HUP & event || POLLNVAL & event;
}

bool isMemoryAllocated(const void *const object) {
    return NULL != object;
}

bool serverMustBeStopped() {
    return STOPPED_PROXY_SERVER == true;
}

void printErrnoMsg(const int errorCode) {
    const int MSG_MAX_SIZE = 256;
    char errMsg[MSG_MAX_SIZE];
    int resultCode;
    resultCode = strerror_r(errorCode, errMsg, MSG_MAX_SIZE);
    if (isErrorOccurred(resultCode)) {
        fprintf(stderr, "STRERROR_R_FAILURE: errno msg %d couldn't be printed.", errorCode);
    } else {
        fprintf(stderr, "Something failed: errno = %d, msg: %s\n", errorCode, errMsg);
    }
}

int initServer() {
    struct sockaddr_in socketAddr;
    memset(&socketAddr, 0, sizeof(socketAddr));

    socketAddr.sin_family = AF_INET;
    socketAddr.sin_port = htons(LISTENING_PORT);
    socketAddr.sin_addr.s_addr = INADDR_ANY;

    int proxyServerDesc = socket(AF_INET, SOCK_STREAM, 0);
    if (isTheErrorSocketResult(proxyServerDesc)) {
        perror("PROXY_SOCKET_ERR: Error creating socket.");
        exit(EXIT_FAILURE);
    }

    int reuseAddrFlag = 1;
    if (setsockopt(proxyServerDesc, SOL_SOCKET, SO_REUSEADDR, &reuseAddrFlag, sizeof(reuseAddrFlag)) ==
        SETSOCKOPT_ERROR) {
        printf("SET_SOCKET_OPT_ERROR");
        exit(EXIT_FAILURE);
    }

    if ((bind(proxyServerDesc, (struct sockaddr *) &socketAddr, sizeof(struct sockaddr))) == BIND_ERROR) {
        perror("PROXY_BIND_ERR: Error while executing a function BIND.");
        close(proxyServerDesc);
        exit(EXIT_FAILURE);
    }

    if (listen(proxyServerDesc, MAX_QUEUE_CLIENTS_NUMBER) < 0) {
        perror("LISTEN_ERR: Error while executing a function LISTEN.");
        close(proxyServerDesc);
        exit(EXIT_FAILURE);
    }

    return proxyServerDesc;
}

void destroyStructuralUrl(structuralUrl *url) {
    free(url->scheme);
    free(url->host);
    free(url->path);
}

void initStructuralUrl(structuralUrl *const url) {
    url->scheme = NULL;
    url->host = NULL;
    url->path = NULL;
}

int parseUrl(const char *const url, structuralUrl *const resultUrl) {
    initStructuralUrl(resultUrl);

    regex_t compiledRegex;
    int resultCode;
    resultCode = regcomp(&compiledRegex, URL_REGEX, REG_EXTENDED);
    if (isErrorOccurred(resultCode)) {
        printErrnoMsg(resultCode);
        return PARSE_URL_FAILURE;
    }

    regmatch_t matchBeginIndexes[REGEX_MATCHES_NUMBER];
    int regexecFlags = 0;
    resultCode = regexec(&compiledRegex, url, REGEX_MATCHES_NUMBER, matchBeginIndexes, regexecFlags);

    if (resultCode == REG_NOMATCH) {
        fprintf(stderr, "SYNTAX_ERROR: incorrect url - %s\n", url);
        return PARSE_URL_FAILURE;
    }

    for (matchesTypes matchType = SCHEME; matchType <= PATH; ++matchType) {
        int beginPosIndex = matchBeginIndexes[matchType].rm_so;
        int endPosIndex = matchBeginIndexes[matchType].rm_eo;

        char *matchedDataPart = NULL;
        if (beginPosIndex != endPosIndex) {
            int dataPartLen = endPosIndex - beginPosIndex;
            matchedDataPart = (char *) malloc(dataPartLen + 1);
            if (!isMemoryAllocated(matchedDataPart)) {
                printErrnoMsg(errno);
                destroyStructuralUrl(resultUrl);
                return PARSE_URL_FAILURE;
            }
            strncpy(matchedDataPart, url + beginPosIndex, dataPartLen);
            matchedDataPart[dataPartLen] = '\0';
        }

        switch (matchType) {
            case SCHEME:
                resultUrl->scheme = matchedDataPart;
                break;
            case HOST:
                resultUrl->host = matchedDataPart;
                break;
            case PATH:
                resultUrl->path = matchedDataPart;
                break;
            default:
                break;
        }
    }
    return PARSE_URL_SUCCESS;
}

int connectToRemoteHost(const char *const hostUrl) {
    struct addrinfo selectedHints;
    struct addrinfo *foundHostAddress = NULL;

    memset(&selectedHints, 0, sizeof(selectedHints));
    selectedHints.ai_family = AF_UNSPEC;
    selectedHints.ai_socktype = SOCK_STREAM;

    int resultCode;
    resultCode = getaddrinfo(hostUrl, SUPPORTED_SERVICE, &selectedHints, &foundHostAddress);
    if (isErrorOccurred(resultCode)) {
        fprintf(stderr, "%s\n", gai_strerror(resultCode));
        if (isMemoryAllocated(foundHostAddress)) {
            freeaddrinfo(foundHostAddress);
        }
        return CONNECT_HOST_ERROR;
    }

    int hostSocket = socket(foundHostAddress->ai_family, foundHostAddress->ai_socktype,
                            foundHostAddress->ai_protocol);

    if (hostSocket == CONNECT_HOST_ERROR) {
        printErrnoMsg(errno);
        freeaddrinfo(foundHostAddress);
        return CONNECT_HOST_ERROR;
    }

    resultCode = connect(hostSocket, foundHostAddress->ai_addr, foundHostAddress->ai_addrlen);
    freeaddrinfo(foundHostAddress);
    if (isErrorOccurred(resultCode)) {
        printErrnoMsg(errno);
        close(hostSocket);
        return CONNECT_HOST_ERROR;
    }

    return hostSocket;
}

void freeClientInfo(Client *client) {
    free(client->info->response);
    destroyStructuralUrl(client->info->URL);

    free(client->info);
}

Cache *initCache() {
    Cache *cache = (Cache *) malloc(sizeof(Cache));
    cache->records = (CacheRecord **) calloc(CACHE_SIZE, sizeof(CacheRecord *));

    for (int i = 0; i < CACHE_SIZE; ++i) {
        cache->records[i] = NULL;
    }
    cache->freeRecordsNumber = CACHE_SIZE;

    return cache;
}

int calcHash(char *url) {
    unsigned long long hashValue = 0;
    int multCoef = 1;

    for (int i = 0; i < strlen(url); ++i) {
        hashValue += url[i] * multCoef;
        multCoef *= P;
    }

    return (int) (hashValue % CACHE_SIZE);
}

CacheRecord *createNewCacheRecord(char *url) {
    CacheRecord *newRecord = (CacheRecord *) malloc(sizeof(CacheRecord));
    newRecord->URL = (char *) malloc(strlen(url));
    newRecord->URL = strcpy(newRecord->URL, url);
    newRecord->response = NULL;

    return newRecord;
}

bool isFreePlaceInCache(unsigned int freePlacesNumber) {
    return freePlacesNumber != 0;
}

bool isFreeCacheRecord(const Cache *cache, const int recordIndex) {
    return cache->records[recordIndex] == NULL;
}

bool areUrlsEquals(const char *const str1, const char *const str2) {
    return !strcmp(str1, str2);
}

int findFreeRecordIndex(Cache *cache, const int beginIndex) {
    for (int i = beginIndex + 1; i != beginIndex; ++i) {
        int curIndex = i % CACHE_SIZE;
        if (cache->records[curIndex] == NULL) {
            return curIndex;
        }
    }

    printf("\nPROXY: Something went wrong in the function findFreeRecordIndex()...\n");
    return NO_FREE_PLACE;
}

void deleteCacheRecord(Cache *cache, const int recordIndex) {
    CacheRecord *deletedRecord = cache->records[recordIndex];
    free(deletedRecord->response);
    free(deletedRecord->URL);
    free(deletedRecord);

    cache->records[recordIndex] = NULL;
}

void freeCache(Cache *cache) {
    for (int i = 0; i < CACHE_SIZE; ++i) {
        if (cache->records[i] != NULL) {
            deleteCacheRecord(cache, i);
        }
    }

    free(cache);
}

void addCacheRecord(Cache *cache, const int recordIndex, CacheRecord *record) {
    cache->records[recordIndex] = record;
}

int addNewResponsePart(Cache *cache, const int recordIndex, char *newResponsePart) {
    if (cache->records[recordIndex]->response == NULL) {
        cache->records[recordIndex]->response = calloc(strlen(newResponsePart) + 1, sizeof(char));
    } else {
        cache->records[recordIndex]->response = (char *) realloc(cache->records[recordIndex]->response,
                                                                 (strlen(cache->records[recordIndex]->response) +
                                                                  strlen(newResponsePart) + 1) * sizeof(char));
    }

    if (!isMemoryAllocated(cache->records[recordIndex]->response)) {
        printErrnoMsg(errno);
        return EXIT_FAILURE;
    }

    cache->records[recordIndex]->response = strcat(cache->records[recordIndex]->response, newResponsePart);
    return EXIT_SUCCESS;
}

void printExistedCacheRecords(const Cache *const cache) {
    printf("\n\tCACHE_CONTENT");

    for (int i = 0; i < CACHE_SIZE; ++i) {
        if (cache->records[i] != NULL) {
            printf("\nRecord #%d:\n", i + 1);
            printf("URL: %s\n", cache->records[i]->URL);
            printf("Response:\n%s\n", cache->records[i]->response);
        }
    }

    printf("\n");
}

Client *createNewClient(int clientSocket, Cache **cache) {
    Client *client = (Client *) malloc(sizeof(Client));
    client->cache = cache;

    client->info = (ClientInfo *) malloc(sizeof(ClientInfo));
    client->info->cacheRecordIndex = UNDEFINED_INDEX;

    client->info->URL = (structuralUrl *) malloc(sizeof(structuralUrl));

    // initialize response pointer here to use the function strcat() next without errors handling
    client->info->response = (char *) malloc(sizeof(char));
    client->info->response[0] = '\0';

    client->info->clientSocket = clientSocket;
    client->info->isWaitingForResponse = false;
    client->info->isRequestSent = false;
    client->info->isResponseReceived = false;

    printf("\n\tNew client was created.\n");

    return client;
}

void deleteClient(Client *client) {
    freeClientInfo(client);
    free(client);
}

void sigIntHandler(int sig) {
    STOPPED_PROXY_SERVER = true;
}

void *clientHandler(void *args) {

    printf("\nNew thread was successfully created.\n");

    Client *client = (Client *) args;
    Cache **cache = client->cache;
    ClientInfo *clientInfo = client->info;

    struct pollfd *pollDescriptors = (struct pollfd *) calloc(2, sizeof(struct pollfd));

    if (!isMemoryAllocated(pollDescriptors)) {
        // notify client about an occurred error
        clientInfo->isWaitingForResponse = true;
        clientInfo->response = "ERROR: memory couldn't be allocated.";
        clientInfo->isResponseReceived = true;
        pollDescriptors[0].events = POLL_OUT;
    }

    int timeout = (10 * 60 * 1000); // 10 min -- in msec

    char readDataBuffer[BUFFER_SIZE];

    int trackedDescNumber = 1;                                                  // initially only client_socket
    memset(pollDescriptors, 0, 2);

    pollDescriptors[CLIENT_POLL_INDEX].fd = clientInfo->clientSocket;
    pollDescriptors[CLIENT_POLL_INDEX].events = POLL_IN;

    while (true) {

        int pollStatus;
        pollStatus = poll(pollDescriptors, trackedDescNumber, timeout);

        // in the error case
        if (isIOErrorOccurred(pollStatus)) {
            break;
        }

        // in the timeout case
        if (isTimeoutReached(pollStatus)) {
            break;
        }

        short int clientRevent = pollDescriptors[CLIENT_POLL_INDEX].revents;

        // client has terminated
        if (isSocketDescUnavailable(clientRevent)) {

            if (isWorkWithRemoteHostInProcess(clientInfo->isWaitingForResponse)) {
                close(clientInfo->remoteHostSocket);
                printf("\nRemote socket was closed.\n");
            }

            close(clientInfo->clientSocket);
            printf("\nClient socket was closed.\n");
            printf("Attention: there is the client that has terminated.\n");

            break;
        }

        // the case when we are waiting for the URL from client
        if (hasClientSentData(clientRevent)) {
            int totalReadSymbols = read(clientInfo->clientSocket, readDataBuffer, BUFFER_SIZE - 1);

            // notify client about an occurred error
            if (isIOErrorOccurred(totalReadSymbols)) {
                printErrnoMsg(errno);
                clientInfo->isWaitingForResponse = true;
                clientInfo->response = "ERROR: socket read error.";
                clientInfo->isResponseReceived = true;
                pollDescriptors[CLIENT_POLL_INDEX].events = POLL_OUT;

                continue;
            }

            readDataBuffer[totalReadSymbols] = '\0';

            // for this step we have already got NOT PARSED URL from client
            structuralUrl structuralUrl;
            int parseUrlResult = parseUrl(readDataBuffer, &structuralUrl);

            // notify client about an occurred error
            if (isErrorOccurred(parseUrlResult)) {
                clientInfo->isWaitingForResponse = true;
                clientInfo->isResponseReceived = true;
                clientInfo->response = "ERROR: bad_url.";
                pollDescriptors[CLIENT_POLL_INDEX].events = POLL_OUT;

                continue;
            }

            int urlHash = calcHash(readDataBuffer);

            pthread_mutex_lock(&cacheMutex);
            if (!isFreeCacheRecord(*cache, urlHash)) {
                if (areUrlsEquals((*cache)->records[urlHash]->URL, readDataBuffer)) {
                    // so for this step remote socket WAS already closed
                    int totalWriteSymbols = write(clientInfo->clientSocket, (*cache)->records[urlHash]->response,
                                                  strlen((*cache)->records[urlHash]->response));

                    if (isIOErrorOccurred(totalWriteSymbols)) {
                        printErrnoMsg(errno);
                        printf("WRITE_CLIENT_SOCKET_FAILURE: client - %d.\n", clientInfo->clientSocket);
                        close(clientInfo->clientSocket);
                        break;
                    }
                } else {
                    CacheRecord *newRecord = createNewCacheRecord(readDataBuffer);

                    if (isFreePlaceInCache((*cache)->freeRecordsNumber)) {
                        int recordIndex = findFreeRecordIndex(*cache, urlHash);

                        if (recordIndex == NO_FREE_PLACE) {
                            printf("FIND_FREE_INDEX_ERROR\n");

                            // notify client about an occurred error
                            clientInfo->isWaitingForResponse = true;
                            clientInfo->isResponseReceived = true;
                            clientInfo->response = "FIND_FREE_INDEX_ERROR";
                            pollDescriptors[CLIENT_POLL_INDEX].events = POLL_OUT;

                            continue;
                        }

                        addCacheRecord(*cache, recordIndex, newRecord);
                        clientInfo->cacheRecordIndex = recordIndex;
                        (*cache)->freeRecordsNumber--;
                    } else {
                        deleteCacheRecord(*cache, urlHash % CACHE_SIZE);
                        addCacheRecord(*cache, urlHash % CACHE_SIZE, newRecord);
                        clientInfo->cacheRecordIndex = urlHash;
                    }
                }
            } else {
                CacheRecord *newRecord = createNewCacheRecord(readDataBuffer);
                addCacheRecord(*cache, urlHash % CACHE_SIZE, newRecord);
                clientInfo->cacheRecordIndex = urlHash % CACHE_SIZE;
                (*cache)->freeRecordsNumber--;
            }
            pthread_mutex_unlock(&cacheMutex);

            // for this step we have already had PARSED URL from client
            fprintf(stdout, "Client URL is successfully parsed.\n Connecting to: %s...\n", structuralUrl.host);
            clientInfo->URL = &structuralUrl;
            clientInfo->isWaitingForResponse = true;
            // now the client is waiting for response
            // that why we don't track him until the time when response will not be received by proxy from the remote host
            pollDescriptors[CLIENT_POLL_INDEX].events = NO_EVENT;
            pollDescriptors[CLIENT_POLL_INDEX].revents = NO_REVENT;

            int remoteHostSocket = connectToRemoteHost(structuralUrl.host);

            if (CONNECT_HOST_ERROR == remoteHostSocket) {
                printf("CONNECT_HOST_ERROR: client - %d.\n", clientInfo->clientSocket);

                clientInfo->isWaitingForResponse = true;
                clientInfo->isResponseReceived = true;
                clientInfo->response = "ERROR: failed to connect to remote host.";
                // todo: добавила строку ниже, логично, что она здесь нужна?
                pollDescriptors[CLIENT_POLL_INDEX].events = POLL_OUT | POLLWRBAND | POLLWRNORM;

                continue;
            }

            // for this step we have already connected to the remote host successfully
            clientInfo->remoteHostSocket = remoteHostSocket;

            pollDescriptors[REMOTE_HOST_POLL_INDEX].fd = remoteHostSocket;
            // now we will wait when remote host be ready to receive request from proxy
            pollDescriptors[REMOTE_HOST_POLL_INDEX].events = POLL_OUT | POLLWRBAND | POLLWRNORM;
            ++trackedDescNumber;

            printf("The \"Client-Proxy-Remote Host\" sequence was successfully created. Remote host - %d.\n",
                   clientInfo->remoteHostSocket);

            continue;

        }


        // client has already sent url and now is waiting for response
        if (isWorkWithRemoteHostInProcess(clientInfo->isWaitingForResponse)) {
            // request was not sent yet => try to send the request to remote host
            if (!hasHttpRequestBeenSent(clientInfo->isRequestSent)) {
                if (isHostReadyToReceiveRequest(pollDescriptors[REMOTE_HOST_POLL_INDEX].revents)) {
                    // sending http-request (GET)
                    printf("Sending request...\n");
                    char *requestedPath = "/";
                    if (isMemoryAllocated(clientInfo->URL->path)) {
                        requestedPath = clientInfo->URL->path;
                    }

                    char fullRequest[FULL_REQUEST_MAX_LENGTH];
                    sprintf(fullRequest,
                            "GET %s HTTP/%s\r\n"
                            "Host: %s\r\n"
                            "Connection: close\r\n"
                            "\r\n", requestedPath, HTTP_SUPPORTED_VERSION,
                            clientInfo->URL->host);

                    printf("FULL_REQUEST: %s\n", fullRequest);

                    int totalWriteSymbols = write(clientInfo->remoteHostSocket, fullRequest,
                                                  strlen(fullRequest));

                    if (isIOErrorOccurred(totalWriteSymbols)) {
                        printErrnoMsg(errno);
                        printf("WRITE_REMOTE_HOST_FAILURE: remote host - %d.\n",
                               clientInfo->remoteHostSocket);

                        close(clientInfo->remoteHostSocket);
                        printf("\nRemote socket was closed.\n");
                        pollDescriptors[REMOTE_HOST_POLL_INDEX].fd = IS_TERMINATED;
                        pollDescriptors[REMOTE_HOST_POLL_INDEX].events = NO_EVENT;
                        pollDescriptors[REMOTE_HOST_POLL_INDEX].revents = NO_REVENT;

                        clientInfo->response = "ERROR: failed to send http-request to remote host.";
                        clientInfo->isResponseReceived = true;

                        continue;
                    }

                    clientInfo->isRequestSent = true;
                    // now the proxy is waiting for a response from the remote host
                    pollDescriptors[REMOTE_HOST_POLL_INDEX].events = POLL_IN;

                }

                continue;

            } else {                // request was already sent

                // else work with the remote host HAS NOT finished, in process: proxy is waiting for the response from the remote host
                // read data from the remote host and try to send it to the client immediately
                if (hasRemoteHostSentResponse(pollDescriptors[REMOTE_HOST_POLL_INDEX].revents)) {
                    int totalReadSymbols = 0;

                    if ((totalReadSymbols = read(clientInfo->remoteHostSocket, readDataBuffer, BUFFER_SIZE - 1)) !=
                        END_OF_DATA) {

                        // error occurred
                        if (isIOErrorOccurred(totalReadSymbols)) {
                            printErrnoMsg(errno);
                            fflush(stdout);

                            close(clientInfo->remoteHostSocket);
                            printf("\nRemote socket was closed.\n");
                            pollDescriptors[REMOTE_HOST_POLL_INDEX].fd = IS_TERMINATED;
                            pollDescriptors[REMOTE_HOST_POLL_INDEX].events = NO_EVENT;
                            pollDescriptors[REMOTE_HOST_POLL_INDEX].revents = NO_REVENT;

                            // notify client about an occurred error
                            clientInfo->response = "ERROR: host socket read error.";
                            clientInfo->isResponseReceived = true;
                            pollDescriptors[CLIENT_POLL_INDEX].events = POLL_OUT;

                            continue;

                        } else {
                            // error doesn't occurred
                            readDataBuffer[totalReadSymbols] = '\0';

                            int resultCode = addNewResponsePart(*cache, clientInfo->cacheRecordIndex, readDataBuffer);

                            if (resultCode == EXIT_FAILURE) {
                                printf("\nRemote socket was closed.\n");
                                pollDescriptors[REMOTE_HOST_POLL_INDEX].fd = IS_TERMINATED;
                                pollDescriptors[REMOTE_HOST_POLL_INDEX].revents = NO_REVENT;

                                // notify client about an occurred error
                                clientInfo->response = "ERROR: failed to allocate memory.";
                                client->info->isResponseReceived = true;

                                pollDescriptors[CLIENT_POLL_INDEX].events = POLL_OUT;

                                continue;
                            }

                            int totalWriteSymbols = write(clientInfo->clientSocket, readDataBuffer,
                                                          strlen(readDataBuffer));

                            if (isIOErrorOccurred(totalWriteSymbols)) {
                                printErrnoMsg(errno);
                                printf("WRITE_CLIENT_SOCKET_FAILURE: client - %d.\n",
                                       clientInfo->clientSocket);

                                close(clientInfo->remoteHostSocket);
                                printf("\nRemote socket was closed.\n");

                                close(clientInfo->clientSocket);
                                printf("\nClient socket was closed.\n");

                                break;
                            }

                        }

                    } else {
                        close(clientInfo->remoteHostSocket);
                        printf("\nRemote socket was closed.\n");
                        pollDescriptors[REMOTE_HOST_POLL_INDEX].fd = IS_TERMINATED;
                        pollDescriptors[REMOTE_HOST_POLL_INDEX].events = NO_EVENT;
                        pollDescriptors[REMOTE_HOST_POLL_INDEX].revents = NO_REVENT;

                        close(clientInfo->clientSocket);
                        printf("\nClient socket was closed.\n");
                        pollDescriptors[CLIENT_POLL_INDEX].fd = IS_TERMINATED;
                        pollDescriptors[CLIENT_POLL_INDEX].events = NO_EVENT;
                        pollDescriptors[CLIENT_POLL_INDEX].revents = NO_REVENT;

                        break;
                    }
                }

                continue;

            }
        }
        // the case when the proxy is waiting url from the client, but still the client has not sent it => just wait url, skip this client and process next one
        // just continue;
    }

    freeClientInfo(client);
    free(client);

    free(pollDescriptors);

    pthread_mutex_lock(&clientsNumberMutex);
    --clientsNumber;
    pthread_mutex_unlock(&clientsNumberMutex);

    printf("\nTHREAD HAS FINISHED.\n");

    pthread_exit((void *) 0);

}

int main(int argc, char **argv) {

    signal(SIGINT, sigIntHandler);

    Cache *cache = initCache();

    int proxyListeningSocket = initServer();
    printf("Proxy server has initialised.\n");

    int supportedClientsNumber = EXTRA_LOTS_NUMBER;
    struct pollfd *pollDescriptors = (struct pollfd *) calloc(supportedClientsNumber * 2 + 1, sizeof(struct pollfd));

    if (!isMemoryAllocated(pollDescriptors)) {
        printf("SERVER HAS STOPPED...\n");
        freeCache(cache);
        return EXIT_FAILURE;
    }

    int trackedDescNumber = 1;                                                  // initially only "accept"
    memset(pollDescriptors, 0, supportedClientsNumber * 2 + 1);

    pollDescriptors[ACCEPT_DESC_INDEX].fd = proxyListeningSocket;
    pollDescriptors[ACCEPT_DESC_INDEX].events = POLLIN;

    int timeout = (10 * 60 * 1000); // 10 min -- in msec

    int newClientSocket;
    printf("Proxy server start to work.\n");

    pthread_t clients[MAX_CLIENTS_NUMBER];

    pthread_mutex_init(&cacheMutex, NULL);
    pthread_mutex_init(&cacheMutex, NULL);

    /* Loop waiting for incoming connects */
    while (true) {

        if (serverMustBeStopped()) {
            break;
        }

        printExistedCacheRecords(cache);

        printf("\n\nWaiting on poll()...\n");

        for (int i = 0; i < trackedDescNumber; ++i) {
            printf("\npollDescs[%d]: fd = %d event = %d revent = %d\n", i, pollDescriptors[i].fd,
                   pollDescriptors[i].events, pollDescriptors[i].revents);
        }

        int pollStatus;
        pollStatus = poll(pollDescriptors, trackedDescNumber, timeout);

        // in the error case
        if (isIOErrorOccurred(pollStatus)) {
            printErrnoMsg(errno);
            STOPPED_PROXY_SERVER = true;
            printf("SERVER HAS STOPPED...\n");
            break;
        }

        // in the timeout case
        if (isTimeoutReached(pollStatus)) {
            printf("ERROR: poll() time out...\n");
            STOPPED_PROXY_SERVER = true;
            printf("SERVER HAS STOPPED...\n");
            break;
        }

        /* Accept all incoming connections that are            */
        /* queued up on the listening socket before we         */
        /* loop back and call poll again.                      */

        // case if the accept_socket is unavailable
        if (isSocketDescUnavailable(pollDescriptors[ACCEPT_DESC_INDEX].revents)) {
            printf("ERROR: accept socket poll_error...\n");
            STOPPED_PROXY_SERVER = true;
            printf("SERVER HAS STOPPED...\n");
            break;
        }

        if (isNewConnectionRequest(pollDescriptors[ACCEPT_DESC_INDEX].revents)) {

            printf("Listening proxy socket accept new incoming connections...\n");

            // extend the poll-array if it is necessary
            if (!isThereFreePlaceForAcceptingClient(trackedDescNumber, supportedClientsNumber)) {
                pollDescriptors = (struct pollfd *) realloc(pollDescriptors, sizeof(struct pollfd) *
                                                                             (supportedClientsNumber +
                                                                              EXTRA_LOTS_NUMBER));
                supportedClientsNumber += EXTRA_LOTS_NUMBER;
            }

            // accept new connection
            newClientSocket = accept(proxyListeningSocket, NULL, NULL);

            if (isTheErrorAcceptResult(newClientSocket)) {
                perror("PROXY_ACCEPT_ERR: Error while executing a function ACCEPT.");
                STOPPED_PROXY_SERVER = true;
                printf("SERVER HAS STOPPED...\n");
                break;
            }

            Client *client = createNewClient(newClientSocket, &cache);

            if (!isMemoryAllocated(client)) {
                printErrnoMsg(errno);
                close(newClientSocket);
                continue;
            }

            if (pthread_create(&(clients[clientsNumber]), NULL, clientHandler, client) != PTHREAD_CREATE_SUCCESS) {
                close(newClientSocket);
                deleteClient(client);
                continue;
            }

            pthread_mutex_lock(&clientsNumberMutex);
            ++clientsNumber;
            pthread_mutex_unlock(&clientsNumberMutex);

            printf("New incoming connection - %d\n", newClientSocket);

        } else {
            printf("Accept_socket: no new incoming connections.\n");
        }

    }

    freeCache(cache);
    free(pollDescriptors);
    pthread_mutex_destroy(&cacheMutex);
    pthread_mutex_destroy(&clientsNumberMutex);

    return EXIT_SUCCESS;

}
