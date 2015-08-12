#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "MQTTClient.h"
#include "evrythng.h"
#include "evrythng_platform.h"
#include "evrythng_tls_certificate.h"

#define TOPIC_MAX_LEN 128
#define USERNAME "authorization"

static evrythng_return_t evrythng_connect_internal(evrythng_handle_t handle);


typedef struct sub_callback_t {
    char*                   topic;
    int                     qos;
    sub_callback*           callback;
    struct sub_callback_t*  next;
} sub_callback_t;


struct evrythng_ctx_t {
    char*   host;
    int     port;
    char*   client_id;
    char*   key;
    const char* ca_buf;
    size_t  ca_size;
    int     secure_connection;
    int     qos;
    int     initialized;

    unsigned char serialize_buffer[1024];
    unsigned char read_buffer[1024];

    evrythng_log_callback log_callback;
    connection_lost_callback conlost_callback;

    Network     mqtt_network;
    MQTTClient  mqtt_client;
    MQTTPacket_connectData  mqtt_conn_opts;

    sub_callback_t *sub_callbacks;
};


static void evrythng_log(evrythng_handle_t handle, evrythng_log_level_t level, const char* fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    if (handle && handle->log_callback)
        handle->log_callback(level, fmt, vl);
    va_end(vl);
}
#define debug(fmt, ...) evrythng_log(handle, EVRYTHNG_LOG_DEBUG, fmt,  ##__VA_ARGS__);
#define warning(fmt, ...) evrythng_log(handle, EVRYTHNG_LOG_WARNING, fmt,  ##__VA_ARGS__);
#define error(fmt, ...) evrythng_log(handle, EVRYTHNG_LOG_ERROR, fmt,  ##__VA_ARGS__);


evrythng_return_t evrythng_init_handle(evrythng_handle_t* handle)
{
    if (!handle) 
        return EVRYTHNG_BAD_ARGS;

    *handle = (evrythng_handle_t)platform_malloc(sizeof(struct evrythng_ctx_t));

    if (!*handle) 
        return EVRYTHNG_MEMORY_ERROR;

    memset(*handle, 0, sizeof(struct evrythng_ctx_t));
    memcpy(&(*handle)->mqtt_conn_opts, &(MQTTPacket_connectData)MQTTPacket_connectData_initializer, sizeof(MQTTPacket_connectData));

    (*handle)->mqtt_conn_opts.MQTTVersion = 3;
    (*handle)->mqtt_conn_opts.keepAliveInterval = 10;
    (*handle)->mqtt_conn_opts.cleansession = 1;
    (*handle)->mqtt_conn_opts.willFlag = 0;
    (*handle)->mqtt_conn_opts.username.cstring = USERNAME;

    (*handle)->qos = 1;
    (*handle)->log_callback = 0;
    (*handle)->conlost_callback = 0;

    (*handle)->ca_buf = cert_buffer;
    (*handle)->ca_size = sizeof cert_buffer;

	MQTTClientInit(
            &(*handle)->mqtt_client, 
            &(*handle)->mqtt_network, 
            5000, 
            (*handle)->serialize_buffer, sizeof((*handle)->serialize_buffer), 
            (*handle)->read_buffer, sizeof((*handle)->read_buffer));

    return EVRYTHNG_SUCCESS;
}


void evrythng_destroy_handle(evrythng_handle_t handle)
{
    if (!handle) return;
    if (handle->initialized && MQTTisConnected(&handle->mqtt_client)) evrythng_disconnect(handle);
    if (handle->host) platform_free(handle->host);
    if (handle->key) platform_free(handle->key);
    //if (handle->ca_buf) platform_free(handle->ca_buf);
    if (handle->client_id) platform_free(handle->client_id);

    sub_callback_t **_sub_callback = &handle->sub_callbacks;
    while(*_sub_callback) 
    {
        sub_callback_t* _sub_callback_tmp = *_sub_callback;
        _sub_callback = &(*_sub_callback)->next;
        platform_free(_sub_callback_tmp->topic);
        platform_free(_sub_callback_tmp);
    }

    MQTTClientDeinit(&handle->mqtt_client);

    platform_free(handle);
}


static int replace_str(char** dest, const char* src, size_t size)
{
    if (*dest) 
        platform_free(*dest);

    *dest = (char*)platform_malloc(size+1); 
    if (!*dest) 
        return EVRYTHNG_MEMORY_ERROR;
    memset(*dest, 0, size+1);

    strncpy(*dest, src, size);

    return EVRYTHNG_SUCCESS;
}


evrythng_return_t evrythng_set_url(evrythng_handle_t handle, const char* url)
{
    if (!handle || !url)
        return EVRYTHNG_BAD_ARGS;

    if (strncmp("tcp", url, strlen("tcp")) == 0) 
    {
        debug("setting TCP connection %s", url);
        handle->secure_connection = 0;
    }
    else if (strncmp("ssl", url, strlen("ssl")) == 0) 
    {
        debug("setting SSL connection %s", url);
        handle->secure_connection = 1;
    }
    else return EVRYTHNG_BAD_URL;

    char* delim = strrchr(url, ':');
    if (!delim)
    {
        error("url does not contain port");
        return EVRYTHNG_BAD_URL;
    }

    int port = strtol(delim+1, 0, 10);
    if (port <= 0 || port >= 65536)
    {
        error("url does not contain valid port number");
        return EVRYTHNG_BAD_URL;
    }
    handle->port = port;

    const char* host_ptr = url + strlen("tcp://");

    return replace_str(&handle->host, host_ptr, delim - host_ptr);
}


evrythng_return_t evrythng_set_key(evrythng_handle_t handle, const char* key)
{
    if (!handle || !key)
        return EVRYTHNG_BAD_ARGS;
    int r = replace_str(&handle->key, key, strlen(key));
    handle->mqtt_conn_opts.password.cstring = handle->key;
    return r;
}


evrythng_return_t evrythng_set_client_id(evrythng_handle_t handle, const char* client_id)
{
    if (!handle || !client_id)
        return EVRYTHNG_BAD_ARGS;
    int r = replace_str(&handle->client_id, client_id, strlen(client_id));
    handle->mqtt_conn_opts.clientID.cstring = handle->client_id;
    return r;
}


evrythng_return_t evrythng_set_log_callback(evrythng_handle_t handle, evrythng_log_callback callback)
{
    if (!handle)
        return EVRYTHNG_BAD_ARGS;

    handle->log_callback = callback;

    return EVRYTHNG_SUCCESS;
}


evrythng_return_t evrythng_set_conlost_callback(evrythng_handle_t handle, connection_lost_callback callback)
{
    if (!handle)
        return EVRYTHNG_BAD_ARGS;

    handle->conlost_callback = callback;

    return EVRYTHNG_SUCCESS;
}


evrythng_return_t evrythng_set_qos(evrythng_handle_t handle, int qos)
{
    if (!handle || qos < 0 || qos > 2)
        return EVRYTHNG_BAD_ARGS;

    handle->qos = qos;

    return EVRYTHNG_SUCCESS;
}


static sub_callback_t* add_sub_callback(evrythng_handle_t handle, char* topic, int qos, sub_callback *callback)
{
    sub_callback_t **_sub_callbacks = &handle->sub_callbacks;
    while (*_sub_callbacks) 
    {
        _sub_callbacks = &(*_sub_callbacks)->next;
    }

    if ((*_sub_callbacks = (sub_callback_t*)platform_malloc(sizeof(sub_callback_t))) == NULL) 
    {
        return 0;
    }

    if (((*_sub_callbacks)->topic = (char*)platform_malloc(strlen(topic) + 1)) == NULL) 
    {
        platform_free(*_sub_callbacks);
        return 0;
    }

    strcpy((*_sub_callbacks)->topic, topic);
    (*_sub_callbacks)->qos = qos;
    (*_sub_callbacks)->callback = callback;
    (*_sub_callbacks)->next = 0;

    return *_sub_callbacks;
}


static void rm_sub_callback(evrythng_handle_t handle, const char* topic)
{
    sub_callback_t *_sub_callback = handle->sub_callbacks;

    if (!_sub_callback) 
        return;

    if (strcmp(_sub_callback->topic, topic) == 0) 
    {
        handle->sub_callbacks = _sub_callback->next;
        platform_free(_sub_callback->topic);
        platform_free(_sub_callback);
        return;
    }

    while (_sub_callback->next) 
    {
        if (strcmp(_sub_callback->next->topic, topic) == 0) 
        {
            sub_callback_t* _sub_callback_tmp = _sub_callback->next;

            _sub_callback->next = _sub_callback->next->next;

            platform_free(_sub_callback_tmp->topic);
            platform_free(_sub_callback_tmp);
            continue;
        }
        _sub_callback = _sub_callback->next;
    }
}



void message_callback(MessageData* data, void* userdata)
{
    evrythng_handle_t handle = (evrythng_handle_t)userdata;

    //debug("received msg topic: %s", data->topicName->lenstring.data);
    //debug("received msg, length %d, payload: %s", data->message->payloadlen, (char*)data->message->payload);

    if (data->message->payloadlen < 3) 
    {
        error("incorrect message lenth %d", data->message->payloadlen);
        return;
    }

    sub_callback_t *_sub_callback = handle->sub_callbacks;
    while (_sub_callback) 
    {
        if (MQTTPacket_equals(data->topicName, _sub_callback->topic) || MQTTisTopicMatched(_sub_callback->topic, data->topicName))
        {
            (*(_sub_callback->callback))(data->message->payload, data->message->payloadlen);
        }
        _sub_callback = _sub_callback->next;
    }
}


void evrythng_message_cycle(evrythng_handle_t handle, int timeout_ms)
{
    int rc = MQTTYield(&handle->mqtt_client, timeout_ms);
    if (rc == MQTT_CONNECTION_LOST)
    {
        warning("mqtt server connection lost");
        evrythng_disconnect(handle);
        if (handle->conlost_callback)
            (*handle->conlost_callback)(handle);
    }
}


evrythng_return_t evrythng_connect(evrythng_handle_t handle)
{
    if (!handle)
        return EVRYTHNG_BAD_ARGS;

    if (handle->initialized)
        return evrythng_connect_internal(handle);

    if (handle->secure_connection)
        NetworkSecuredInit(&handle->mqtt_network, handle->ca_buf, handle->ca_size);
    else
        NetworkInit(&handle->mqtt_network);

    if (!handle->client_id)
    {
        int i;
        handle->client_id = (char*)platform_malloc(10);
        if (!handle->client_id)
            return EVRYTHNG_MEMORY_ERROR;
        memset(handle->client_id, 0, 10);

        for (i = 0; i < 9; i++)
            handle->client_id[i] = '0' + rand() % 10;
        handle->mqtt_conn_opts.clientID.cstring = handle->client_id;
        debug("client ID: %s", handle->client_id);
    }

    handle->initialized = 1;

    return evrythng_connect_internal(handle);
}


evrythng_return_t evrythng_connect_internal(evrythng_handle_t handle)
{
    int rc;

    if (MQTTisConnected(&handle->mqtt_client))
    {
        warning("already connected");
        return EVRYTHNG_SUCCESS;
    }

    debug("connecting to host: %s, port: %d", handle->host, handle->port);

	if (NetworkConnect(&handle->mqtt_network, handle->host, handle->port))
    {
        error("Failed to establish network connection");
        return EVRYTHNG_CONNECTION_FAILED;
    }

    debug("network connection established");

    if ((rc = MQTTConnect(&handle->mqtt_client, &handle->mqtt_conn_opts)) != MQTT_SUCCESS)
    {
        error("Failed to connect, return code %d", rc);
        NetworkDisconnect(&handle->mqtt_network);
        return EVRYTHNG_CONNECTION_FAILED;
    }
    debug("MQTT connected");

    sub_callback_t **_sub_callbacks = &handle->sub_callbacks;
    while (*_sub_callbacks) 
    {
        int rc = MQTTSubscribe(
                &handle->mqtt_client, 
                (*_sub_callbacks)->topic, 
                (*_sub_callbacks)->qos,
                message_callback,
                handle);
        if (rc >= 0) 
        {
            debug("successfully subscribed to %s", (*_sub_callbacks)->topic);
        }
        else 
        {
            error("subscription failed, rc = %d", rc);
        }
        _sub_callbacks = &(*_sub_callbacks)->next;
    }

    return EVRYTHNG_SUCCESS;
}


evrythng_return_t evrythng_disconnect(evrythng_handle_t handle)
{
    int rc;

    if (!handle)
        return EVRYTHNG_BAD_ARGS;

    if (!handle->initialized)
        return EVRYTHNG_SUCCESS;

    if (!MQTTisConnected(&handle->mqtt_client))
        return EVRYTHNG_SUCCESS;

    sub_callback_t **_sub_callbacks = &handle->sub_callbacks;
    while (*_sub_callbacks) 
    {
        rc = MQTTUnsubscribe(&handle->mqtt_client, (*_sub_callbacks)->topic);
        if (rc >= 0) 
        {
            debug("successfully unsubscribed from %s", (*_sub_callbacks)->topic);
        }
        else 
        {
            warning("unsubscription failed, rc = %d", rc);
        }
        _sub_callbacks = &(*_sub_callbacks)->next;
    }

    rc = MQTTDisconnect(&handle->mqtt_client);
    if (rc != MQTT_SUCCESS)
    {
        error("failed to disconnect mqtt: rc = %d", rc);
    }
    NetworkDisconnect(&handle->mqtt_network);
    debug("MQTT disconnected");

    return EVRYTHNG_SUCCESS;
}


static evrythng_return_t evrythng_publish(
        evrythng_handle_t handle, 
        const char* entity, 
        const char* entity_id, 
        const char* data_type, 
        const char* data_name, 
        const char* property_json)
{
    if (!handle) return EVRYTHNG_BAD_ARGS;

    if (!MQTTisConnected(&handle->mqtt_client)) 
    {
        error("client is not connected");
        return EVRYTHNG_NOT_CONNECTED;
    }

    int rc;
    char pub_topic[TOPIC_MAX_LEN];

    if (entity_id == NULL) 
    {
        rc = snprintf(pub_topic, TOPIC_MAX_LEN, "%s/%s", entity, data_name);
        if (rc < 0 || rc >= TOPIC_MAX_LEN) 
        {
            error("Topic overflow");
            return EVRYTHNG_BAD_ARGS;
        }
    } 
    else if (data_name == NULL) 
    {
        rc = snprintf(pub_topic, TOPIC_MAX_LEN, "%s/%s/%s", entity, entity_id, data_type);
        if (rc < 0 || rc >= TOPIC_MAX_LEN) 
        {
            error("topic overflow");
            return EVRYTHNG_BAD_ARGS;
        }
    } 
    else 
    {
        rc = snprintf(pub_topic, TOPIC_MAX_LEN, "%s/%s/%s/%s", entity, entity_id, data_type, data_name);
        if (rc < 0 || rc >= TOPIC_MAX_LEN) 
        {
            error("topic overflow");
            return EVRYTHNG_BAD_ARGS;
        }
    }

    debug("publish topic: %s", pub_topic);

    MQTTMessage msg = {
        .qos = handle->qos, 
        .retained = 1, 
        .dup = 0,
        .id = 0,
        .payload = (void*)property_json,
        .payloadlen = strlen(property_json)
    };

    rc = MQTTPublish(&handle->mqtt_client, pub_topic, &msg);
    if (rc == MQTT_SUCCESS) 
    {
        debug("published message: %s", property_json);
    }
    else 
    {
        error("could not publish message, rc = %d", rc);
        return EVRYTHNG_PUBLISH_ERROR;
    }

    return EVRYTHNG_SUCCESS;
}


static evrythng_return_t evrythng_subscribe(
        evrythng_handle_t handle, 
        const char* entity, 
        const char* entity_id, 
        const char* data_type, 
        const char* data_name, 
        sub_callback *callback)
{
    if (!MQTTisConnected(&handle->mqtt_client)) 
    {
        error("client is not connected");
        return EVRYTHNG_NOT_CONNECTED;
    }

    int rc;
    char sub_topic[TOPIC_MAX_LEN];

    if (entity_id == NULL) 
    {
        rc = snprintf(sub_topic, TOPIC_MAX_LEN, "%s/%s", entity, data_name);
        if (rc < 0 || rc >= TOPIC_MAX_LEN) 
        {
            debug("topic overflow");
            return EVRYTHNG_BAD_ARGS;
        }
    } 
    else if (data_name == NULL) 
    {
        rc = snprintf(sub_topic, TOPIC_MAX_LEN, "%s/%s/%s", entity, entity_id, data_type);
        if (rc < 0 || rc >= TOPIC_MAX_LEN) 
        {
            debug("topic overflow");
            return EVRYTHNG_BAD_ARGS;
        }
    } 
    else 
    {
        rc = snprintf(sub_topic, TOPIC_MAX_LEN, "%s/%s/%s/%s", entity, entity_id, data_type, data_name);
        if (rc < 0 || rc >= TOPIC_MAX_LEN) 
        {
            debug("topic overflow");
            return EVRYTHNG_BAD_ARGS;
        }
    }

    sub_callback_t* new_callback = add_sub_callback(handle, sub_topic, handle->qos, callback);
    if (!new_callback) 
    {
        error("could not add subscription callback");
        return EVRYTHNG_MEMORY_ERROR;
    }

    debug("subscribing to topic: %s", sub_topic);

    rc = MQTTSubscribe(&handle->mqtt_client, new_callback->topic, handle->qos, message_callback, handle);
    if (rc >= 0) 
    {
        debug("successfully subscribed to %s", sub_topic);
    }
    else 
    {
        debug("subscription failed, rc=%d", rc);

        rm_sub_callback(handle, sub_topic);

        return EVRYTHNG_SUBSCRIPTION_ERROR;
    }

    return EVRYTHNG_SUCCESS;
}


static evrythng_return_t evrythng_unsubscribe(
        evrythng_handle_t handle, 
        const char* entity, 
        const char* entity_id, 
        const char* data_type, 
        const char* data_name)
{
    if (!MQTTisConnected(&handle->mqtt_client)) 
    {
        error("client is not connected");
        return EVRYTHNG_NOT_CONNECTED;
    }

    int rc;
    char unsub_topic[TOPIC_MAX_LEN];

    if (entity_id == NULL) 
    {
        rc = snprintf(unsub_topic, TOPIC_MAX_LEN, "%s/%s", entity, data_name);
        if (rc < 0 || rc >= TOPIC_MAX_LEN) 
        {
            debug("topic overflow");
            return EVRYTHNG_BAD_ARGS;
        }
    } 
    else if (data_name == NULL) 
    {
        rc = snprintf(unsub_topic, TOPIC_MAX_LEN, "%s/%s/%s", entity, entity_id, data_type);
        if (rc < 0 || rc >= TOPIC_MAX_LEN) 
        {
            debug("topic overflow");
            return EVRYTHNG_BAD_ARGS;
        }
    } 
    else 
    {
        rc = snprintf(unsub_topic, TOPIC_MAX_LEN, "%s/%s/%s/%s", entity, entity_id, data_type, data_name);
        if (rc < 0 || rc >= TOPIC_MAX_LEN) 
        {
            debug("topic overflow");
            return EVRYTHNG_BAD_ARGS;
        }
    }

    rm_sub_callback(handle, unsub_topic);

    rc = MQTTUnsubscribe(&handle->mqtt_client, unsub_topic);
    if (rc >= 0) 
    {
        debug("unsubscribed from %s", unsub_topic);
    }
    else 
    {
        error("unsubscription failed, rc=%d", rc);
        return EVRYTHNG_UNSUBSCRIPTION_ERROR;
    }

    return EVRYTHNG_SUCCESS;
}


evrythng_return_t evrythng_publish_thng_property(
        evrythng_handle_t handle, 
        const char* thng_id, 
        const char* property_name, 
        const char* property_json)
{
    if (!thng_id || !property_name || !property_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "thngs", thng_id, "properties", property_name, property_json);
}


evrythng_return_t evrythng_subscribe_thng_property(
        evrythng_handle_t handle, 
        const char* thng_id, 
        const char* property_name, 
        sub_callback *callback)
{
    if (!thng_id || !property_name || !callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "thngs", thng_id, "properties", property_name, callback);
}

evrythng_return_t evrythng_unsubscribe_thng_property(
        evrythng_handle_t handle, 
        const char* thng_id, 
        const char* property_name)
{
    if (!thng_id || !property_name)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "thngs", thng_id, "properties", property_name);
}


evrythng_return_t evrythng_subscribe_thng_properties(
        evrythng_handle_t handle, 
        const char* thng_id, 
        sub_callback *callback)
{
    if (!thng_id || !callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "thngs", thng_id, "properties", NULL, callback);
}


evrythng_return_t evrythng_unsubscribe_thng_properties(
        evrythng_handle_t handle, 
        const char* thng_id)
{
    if (!thng_id)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "thngs", thng_id, "properties", NULL);
}


evrythng_return_t evrythng_publish_thng_properties(
        evrythng_handle_t handle, 
        const char* thng_id, 
        const char* properties_json)
{
    if (!thng_id || !properties_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "thngs", thng_id, "properties", NULL, properties_json);
}


evrythng_return_t evrythng_subscribe_thng_action(
        evrythng_handle_t handle, 
        const char* thng_id, 
        const char* action_name, 
        sub_callback *callback)
{
    if (!thng_id || !action_name || !callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "thngs", thng_id, "actions", action_name, callback);
}


evrythng_return_t evrythng_unsubscribe_thng_action(
        evrythng_handle_t handle, 
        const char* thng_id, 
        const char* action_name)
{
    if (!thng_id || !action_name)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "thngs", thng_id, "actions", action_name);
}


evrythng_return_t evrythng_subscribe_thng_actions(
        evrythng_handle_t handle, 
        const char* thng_id, 
        sub_callback *callback)
{
    if (!thng_id || !callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "thngs", thng_id, "actions", "all", callback);
}


evrythng_return_t evrythng_unsubscribe_thng_actions(
        evrythng_handle_t handle, 
        const char* thng_id)
{
    if (!thng_id)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "thngs", thng_id, "actions", "all");
}


evrythng_return_t evrythng_publish_thng_action(
        evrythng_handle_t handle, 
        const char* thng_id, 
        const char* action_name, 
        const char* action_json)
{
    if (!thng_id || !action_name || !action_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "thngs", thng_id, "actions", action_name, action_json);
}


evrythng_return_t evrythng_publish_thng_actions(
        evrythng_handle_t handle, 
        const char* thng_id, 
        const char* actions_json)
{
    if (!thng_id || !actions_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "thngs", thng_id, "actions", "all", actions_json);
}


evrythng_return_t evrythng_subscribe_thng_location(
        evrythng_handle_t handle, 
        const char* thng_id, 
        sub_callback *callback)
{
    if (!thng_id || !callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "thngs", thng_id, "location", NULL, callback);
}


evrythng_return_t evrythng_unsubscribe_thng_location(
        evrythng_handle_t handle, 
        const char* thng_id)
{
    if (!thng_id)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "thngs", thng_id, "location", NULL);
}


evrythng_return_t evrythng_publish_thng_location(
        evrythng_handle_t handle, 
        const char* thng_id, 
        const char* location_json)
{
    if (!thng_id || !location_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "thngs", thng_id, "location", NULL, location_json);
}


evrythng_return_t evrythng_subscribe_product_property(
        evrythng_handle_t handle, 
        const char* product_id, 
        const char* property_name, 
        sub_callback *callback)
{
    if (!product_id || !property_name || !callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "products", product_id, "properties", property_name, callback);
}


evrythng_return_t evrythng_unsubscribe_product_property(
        evrythng_handle_t handle, 
        const char* product_id, 
        const char* property_name)
{
    if (!product_id || !property_name)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "products", product_id, "properties", property_name);
}


evrythng_return_t evrythng_subscribe_product_properties(
        evrythng_handle_t handle, 
        const char* product_id, 
        sub_callback *callback)
{
    if (!product_id || !callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "products", product_id, "properties", NULL, callback);
}


evrythng_return_t evrythng_unsubscribe_product_properties(
        evrythng_handle_t handle, 
        const char* product_id)
{
    if (!product_id)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "products", product_id, "properties", NULL);
}


evrythng_return_t evrythng_publish_product_property(
        evrythng_handle_t handle, 
        const char* product_id, 
        const char* property_name, 
        const char* property_json)
{
    if (!product_id || !property_name || !property_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "products", product_id, "properties", property_name, property_json);
}


evrythng_return_t evrythng_publish_product_properties(
        evrythng_handle_t handle, 
        const char* product_id, 
        const char* properties_json)
{
    if (!product_id || !properties_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "products", product_id, "properties", NULL, properties_json);
}


evrythng_return_t evrythng_subscribe_product_action(
        evrythng_handle_t handle, 
        const char* product_id, 
        const char* action_name, 
        sub_callback *callback)
{
    if (!product_id || !action_name || !callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "products", product_id, "actions", action_name, callback);
}


evrythng_return_t evrythng_unsubscribe_product_action(
        evrythng_handle_t handle, 
        const char* product_id, 
        const char* action_name)
{
    if (!product_id || !action_name)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "products", product_id, "actions", action_name);
}


evrythng_return_t evrythng_subscribe_product_actions(
        evrythng_handle_t handle, 
        const char* product_id, 
        sub_callback *callback)
{
    if (!product_id || !callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "products", product_id, "actions", "all", callback);
}


evrythng_return_t evrythng_unsubscribe_product_actions(
        evrythng_handle_t handle, 
        const char* product_id)
{
    if (!product_id)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "products", product_id, "actions", "all");
}


evrythng_return_t evrythng_publish_product_action(
        evrythng_handle_t handle, 
        const char* product_id, 
        const char* action_name, 
        const char* action_json)
{
    if (!product_id || !action_name || !action_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "products", product_id, "actions", action_name, action_json);
}


evrythng_return_t evrythng_publish_product_actions(
        evrythng_handle_t handle, 
        const char* product_id, 
        const char* actions_json)
{
    if (!product_id || !actions_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "products", product_id, "actions", "all", actions_json);
}


evrythng_return_t evrythng_subscribe_action(
        evrythng_handle_t handle, 
        const char* action_name, 
        sub_callback *callback)
{
    if (!action_name)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "actions", NULL, NULL, action_name, callback);
}


evrythng_return_t evrythng_unsubscribe_action(
        evrythng_handle_t handle, 
        const char* action_name)
{
    if (!action_name)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_unsubscribe(handle, "actions", NULL, NULL, action_name);
}


evrythng_return_t evrythng_subscribe_actions(evrythng_handle_t handle, sub_callback *callback)
{
    if (!callback)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_subscribe(handle, "actions", NULL, NULL, "all", callback);
}


evrythng_return_t evrythng_unsubscribe_actions(evrythng_handle_t handle)
{
    return evrythng_unsubscribe(handle, "actions", NULL, NULL, "all");
}


evrythng_return_t evrythng_publish_action(
        evrythng_handle_t handle, 
        const char* action_name, 
        const char* action_json)
{
    if (!action_name || !action_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "actions", NULL, NULL, action_name, action_json);
}


evrythng_return_t evrythng_publish_actions(
        evrythng_handle_t handle, 
        const char* actions_json)
{
    if (!actions_json)
        return EVRYTHNG_BAD_ARGS;

    return evrythng_publish(handle, "actions", NULL, NULL, "all", actions_json);
}
