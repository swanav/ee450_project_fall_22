#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>

#include "protocol.h"
#include "data/credentials.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "utils/messages.h"
#include "utils/constants.h"
#include "data/courses.h"
#include "networking/tcp_client.h"

LOG_TAG(client);

#define CLIENT_TEST

typedef struct __client_context_t {
    int auth_failure_count;
    pthread_t user_input_thread;
    pthread_t network_thread;
    sem_t semaphore;
    tcp_client_t *client;
    credentials_t creds;
} client_context_t;

int collect_credentials(credentials_t* user) {
    printf("Enter username: ");

#ifdef CLIENT_TEST
    user->username_len = strlen("james");
    memcpy(user->username, "james", user->username_len);
    printf("james\r\n");
#else
    if (scanf("%s", user->username)) {
        user->username_len = (uint8_t) strlen((char*) user->username);
    }
#endif 

    printf("Enter password: ");
#ifdef CLIENT_TEST
    user->password_len = strlen("2kAnsa7s)");
    memcpy(user->password, "2kAnsa7s)", user->password_len);
    printf("2kAnsa7s)\r\n");
#else
    if (scanf("%s", user->password)) {
        user->password_len = (uint8_t) strlen((char*) user->password);
    }
#endif 

    return 0;
}

static void on_authentication_success(client_context_t* ctx, uint8_t* username, uint8_t username_len) {
    ctx->auth_failure_count = 0;
    LOG_INFO(CLIENT_MESSAGE_ON_AUTH_RESULT_SUCCESS);
}

static void on_authentication_failure(client_context_t* ctx, uint8_t* username, uint8_t username_len, uint8_t flags) {
    LOG_ERR(CLIENT_MESSAGE_ON_AUTH_RESULT_FAILURE);

    if (flags & AUTH_FLAGS_USER_NOT_FOUND) {
        LOG_ERR("User not found");
    } else if (flags & AUTH_FLAGS_PASSWORD_MISMATCH) {
        LOG_ERR("Password mismatch");
    }

    if (++ctx->auth_failure_count == 3) {
        LOG_INFO("Maximum attempts reached. Closing client.");
        exit(1);
    } else {
        LOG_INFO("Please try again... %d attempts remaining", 3 - ctx->auth_failure_count);
    }
}

static void on_auth_result(client_context_t* ctx, tcp_sgmnt_t* sgmnt) {
    LOG_ERR(CLIENT_MESSAGE_ON_AUTH_RESULT, ctx->creds.username_len, ctx->creds.username, ntohs(ctx->client->server->addr.sin_port));
    uint8_t flags = 0;
    uint8_t payload_len = 0;
    protocol_decode(sgmnt, NULL, &flags, &payload_len, 0, NULL);
    if (payload_len == 0) {
        if (AUTH_MASK_SUCCESS(flags)) {
            on_authentication_success(ctx, ctx->creds.username, ctx->creds.username_len);
        } else {
            on_authentication_failure(ctx, ctx->creds.username, ctx->creds.username_len, flags);
        }
    }
    sem_post(&ctx->semaphore);
}

static void authenticate_user(client_context_t* ctx) {
    uint8_t credentials_buffer[sizeof(credentials_t)];
    uint8_t credentials_out_len = 0;
    if (credentials_encode(&ctx->creds, credentials_buffer, sizeof(credentials_buffer), &credentials_out_len)) {
        LOG_ERR("Failed to encode credentials.");
        return;
    }
    tcp_sgmnt_t sgmnt = {0};
    protocol_encode(&sgmnt, REQUEST_TYPE_AUTH, 0, credentials_out_len, credentials_buffer);
    if (tcp_client_send(ctx->client, &sgmnt) == ERR_OK) {
        LOG_INFO(CLIENT_MESSAGE_ON_AUTH_REQUEST, ctx->creds.username_len, ctx->creds.username);
    }
}

static void on_setup_complete(client_context_t* ctx) {
    LOG_INFO(CLIENT_MESSAGE_ON_BOOTUP);
    ctx->client->user_data = ctx;
    sem_post(&ctx->semaphore);
}

int collect_course_codes(char* course_code, uint8_t course_code_buffer_size) {
    fgets(course_code, course_code_buffer_size, stdin);
    return utils_get_word_count(string_trim(course_code));
}

static int new_request_prompt(char* course_code_buffer, uint8_t course_code_buffer_size, char* category_buffer, uint8_t category_buffer_size) {
    printf("Please enter the course code to query: ");
    int courses_count = collect_course_codes(course_code_buffer, course_code_buffer_size);
    if (courses_count == 1) {
        printf("Please enter the category (Credit / Professor / Days / CourseName): ");
        fgets(category_buffer, category_buffer_size, stdin);
    }
    return courses_count;
}

static void send_request(client_context_t* ctx, int courses_count, char* course_code_buffer, uint8_t course_code_buffer_size, char* category_buffer, uint8_t category_buffer_size) {
    tcp_sgmnt_t sgmnt = {0};
    if (courses_count == 1) {
        // LOG_INFO("Course: %s Information Requested: %s", course_code_buffer, category_buffer);
        courses_lookup_params_t params = {0};
        memcpy(params.course_code, course_code_buffer, strlen(course_code_buffer));
        params.category = courses_lookup_category_from_string(string_trim(category_buffer));
        // LOG_INFO("Category: %d", params.category);
        courses_lookup_info_request_encode(&params, &sgmnt);
    } else if (courses_count > 1) {
        LOG_INFO("Printing Course Details for requested course codes. (%s)", course_code_buffer);
        courses_lookup_multiple_request_encode(&sgmnt, courses_count, course_code_buffer, course_code_buffer_size);
    }
    tcp_client_send(ctx->client, &sgmnt);
}

static void on_course_lookup_info(client_context_t* ctx, tcp_sgmnt_t* sgmnt) {
    LOG_INFO("Received course lookup info.");
    courses_lookup_params_t params = {0};
    uint8_t info[64];
    uint8_t info_len = 0;
    uint8_t flags = protocol_get_flags(sgmnt);
    if (COURSE_LOOKUP_MASK_INVALID(flags)) {
        LOG_ERR("Invalid query.");
    } else if (courses_lookup_info_response_decode(sgmnt, &params, info, sizeof(info), &info_len) == ERR_COURSES_OK) {
        LOG_INFO("The %s of %s is %.*s", courses_category_string_from_enum(params.category), params.course_code, info_len, info);   
    }
}

static void on_course_multi_lookup(client_context_t* ctx, tcp_sgmnt_t* sgmnt) {
    LOG_INFO("Received course multi lookup info.");
    courses_lookup_params_t params = {0};
    uint8_t info[64];
    uint8_t info_len = 0;
    uint8_t flags = protocol_get_flags(sgmnt);

    LOG_WARN("Course Code: Credits, Professor, Days, Course Name", params.course_code);

}

static void* user_input_task(void* params) {

    client_context_t* ctx = (client_context_t*) params;
    sem_wait(&ctx->semaphore);

    do {
        bzero(&ctx->creds, sizeof(credentials_t));
        collect_credentials(&ctx->creds);
        authenticate_user(ctx);
        sem_wait(&ctx->semaphore);
    } while(ctx->auth_failure_count != 0);

    char course_code[100] = {0};
    char category[20] = {0};

    while(1) {
        bzero(course_code, sizeof(course_code));
        bzero(category, sizeof(category));
        LOG_INFO("\r\n\r\n------------Start a new request------------");
        int count = new_request_prompt(course_code, sizeof(course_code), category, sizeof(category));
        send_request(ctx, count, course_code, sizeof(course_code), category, sizeof(category));
        sem_wait(&ctx->semaphore);
        LOG_INFO("\r\n------------End of Request------------\r\n");
    }

    return NULL;
}

static void on_tcp_disconnect(tcp_client_t* clientparams) {
    exit(1);
}

static void on_receive(tcp_client_t* client, tcp_sgmnt_t* sgmnt) {
    client_context_t* ctx = (client_context_t*) client->user_data;
    if (sgmnt->data_len == 0) {
        LOG_ERR("Received empty segment.");
        return;
    }
    response_type_t response_type = protocol_get_request_type(sgmnt);
    switch (response_type) {
        case RESPONSE_TYPE_AUTH:
            on_auth_result(ctx, sgmnt);
            break;
        case RESPONSE_TYPE_COURSES_LOOKUP_INFO:
            on_course_lookup_info(ctx, sgmnt);
            sem_post(&ctx->semaphore);
            break;
        case RESPONSE_TYPE_COURSES_MULTI_LOOKUP:
            LOG_INFO("Received course lookup.");
            on_course_multi_lookup(ctx, sgmnt);
            break;
        default:
            LOG_ERR("Unknown segment type. %d", response_type);
            break;
    }
}

static void* network_thread_task(void* params) {
    client_context_t* ctx = (client_context_t*) params;

    tcp_endpoint_t dst = {0};
    SERVER_ADDR_PORT(dst, SERVER_M_TCP_PORT_NUMBER);

    ctx->client = tcp_client_connect(&dst, on_receive, on_tcp_disconnect);
    on_setup_complete(ctx);
    while (1) {
        tcp_client_receive(ctx->client);
    }
    tcp_client_disconnect(ctx->client);
    return NULL;
}


int main() {
    client_context_t ctx = {0};
    sem_init(&ctx.semaphore, 0, 0);
    pthread_create(&ctx.user_input_thread, NULL, &user_input_task, &ctx);
    pthread_create(&ctx.network_thread, NULL, &network_thread_task, &ctx);
    pthread_join(ctx.network_thread, NULL);
    return 0;
}
