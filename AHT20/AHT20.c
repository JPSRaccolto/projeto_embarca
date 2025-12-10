#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "aht20.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "lwip/apps/mqtt.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/dns.h"
#include "lwip/altcp_tls.h"
#include "lib/bh1750_light_sensor.c"

// Configurações Wi-Fi e MQTT
#define WIFI_SSID "computador"
#define WIFI_PASSWORD "12345678*"
#define MQTT_SERVER "192.168.137.211"
#define MQTT_USERNAME "joao"
#define MQTT_PASSWORD "1234"
// Limiar e histerese (ajuste conforme necessidade)
#define LUX_THRESHOLD 100
#define HYSTERESIS 5  // liga quando < (LUX_THRESHOLD - HYSTERESIS), desliga quando > (LUX_THRESHOLD + HYSTERESIS)
#define I2C_PORT i2c0
#define I2C_SDA 0
#define I2C_SCL 1
#define I2C_PORT_BH i2c1
#define I2C_SDA_BH 2
#define I2C_SCL_BH 3
#define RELAY_PIN 15
// Definições MQTT
#define TEMP_WORKER_TIME_S 3
#define MQTT_KEEP_ALIVE_S 60
#define MQTT_PUBLISH_QOS 1
#define MQTT_PUBLISH_RETAIN 0
#define MQTT_WILL_TOPIC "/online"
#define MQTT_WILL_MSG "0"
#define MQTT_WILL_QOS 1
#define MQTT_DEVICE_NAME "pico1"
#define MQTT_UNIQUE_TOPIC 0
#define MQTT_TOPIC_LEN 100
#define MQTT_OUTPUT_RINGBUF_SIZE 256

// Status do sistema
char status_sistema[30] = "Sistema OK";
uint16_t lux = 0;
bool relay_state = false; // false = desligado, true = ligado
// Estrutura de dados do cliente MQTT
typedef struct {
    mqtt_client_t* mqtt_client_inst;
    struct mqtt_connect_client_info_t mqtt_client_info;
    char data[MQTT_OUTPUT_RINGBUF_SIZE];
    char topic[MQTT_TOPIC_LEN];
    uint32_t len;
    ip_addr_t mqtt_server_address;
    bool connect_done;
    int subscribe_count;
    bool stop_client;
} MQTT_CLIENT_DATA_T;

// Protótipos de funções
static void pub_request_cb(__unused void *arg, err_t err);
static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name);
static void temperature_worker_fn(async_context_t *context, async_at_time_worker_t *worker);
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);
static void start_client(MQTT_CLIENT_DATA_T *state);
static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg);
static void publish_all_states(MQTT_CLIENT_DATA_T *state);
static async_at_time_worker_t temperature_worker = { .do_work = temperature_worker_fn };
AHT20_Data data;
float temp_offset = 0.0f;
float umid_offset = 0.0f;
void inicia_pinos(){
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    i2c_init(I2C_PORT_BH, 400 * 1000);
    gpio_set_function(I2C_SDA_BH, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_BH, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_BH);
    gpio_pull_up(I2C_SCL_BH);
    bh1750_power_on(I2C_PORT_BH);
    gpio_init(RELAY_PIN);
    gpio_set_dir(RELAY_PIN, GPIO_OUT);
    gpio_put(RELAY_PIN, 0);
}


// Funções MQTT (implementações simplificadas para compilação)
static void pub_request_cb(__unused void *arg, err_t err) {
    if (err != 0) {
        printf("pub_request_cb failed %d\n", err);
    }
}

static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name) {
#if MQTT_UNIQUE_TOPIC
    static char full_topic_buf[MQTT_TOPIC_LEN];
    snprintf(full_topic_buf, sizeof(full_topic_buf), "/%s%s", state->mqtt_client_info.client_id, name);
    return full_topic_buf;
#else
    return name;
#endif
}

static void publish_all_states(MQTT_CLIENT_DATA_T *state) {
    char buffer[20];

    snprintf(buffer, sizeof(buffer), "%.2f", data.temperature);
    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/temperatura1/state"), buffer, strlen(buffer), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);

    snprintf(buffer, sizeof(buffer), "%.2f", data.humidity);
    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/umidade1/state"), buffer, strlen(buffer), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    
    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/status1/state"), status_sistema, strlen(status_sistema), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    // 3. Luminosidade (Lux)
    snprintf(buffer, sizeof(buffer), "%u", lux);
    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/luminosidade1/state"), buffer, strlen(buffer), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);

    // 4. Estado do Relé (Envia "ON" ou "OFF" - ou "1"/"0" se preferir)
    const char* estado_rele = relay_state ? "ON" : "OFF";
    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/rele1/state"), estado_rele, strlen(estado_rele), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    printf("MQTT: Status publicados.\n");
}

static void temperature_worker_fn(async_context_t *context, async_at_time_worker_t *worker) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)worker->user_data;
    publish_all_states(state); 
    async_context_add_at_time_worker_in_ms(context, worker, TEMP_WORKER_TIME_S * 1000);
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
    if (status == MQTT_CONNECT_ACCEPTED) {
        state->connect_done = true;

        if (state->mqtt_client_info.will_topic) {
            mqtt_publish(state->mqtt_client_inst, state->mqtt_client_info.will_topic, "1", 1, MQTT_WILL_QOS, true, pub_request_cb, state);
        }

        temperature_worker.user_data = state;
        async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &temperature_worker, 0);
    } else if (status == MQTT_CONNECT_DISCONNECTED) {
        if (!state->connect_done) {
            printf("Failed to connect to mqtt server\n");
        }
    }
}

static void start_client(MQTT_CLIENT_DATA_T *state) {
    const int port = MQTT_PORT;
    
    state->mqtt_client_inst = mqtt_client_new();
    if (state->mqtt_client_inst == NULL) {
        printf("Failed to create mqtt client\n");
        return;
    }

    state->mqtt_client_info.client_id = MQTT_DEVICE_NAME;
    state->mqtt_client_info.client_user = MQTT_USERNAME;
    state->mqtt_client_info.client_pass = MQTT_PASSWORD;
    state->mqtt_client_info.keep_alive = MQTT_KEEP_ALIVE_S;
    state->mqtt_client_info.will_topic = MQTT_WILL_TOPIC;
    state->mqtt_client_info.will_msg = MQTT_WILL_MSG;
    state->mqtt_client_info.will_qos = MQTT_WILL_QOS;
    state->mqtt_client_info.will_retain = 1;

    err_t err = mqtt_client_connect(state->mqtt_client_inst, &state->mqtt_server_address, port, mqtt_connection_cb, state, &state->mqtt_client_info);
    if (err != ERR_OK) {
        printf("mqtt_connect return: %d\n", err);
    }
}

static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T*)arg;
    if (ipaddr) {
        state->mqtt_server_address = *ipaddr;
        printf("DNS resolved %s -> %s\n", hostname, ip4addr_ntoa(ipaddr));
        start_client(state);
    } else {
        printf("DNS failed to resolve %s\n", hostname);
    }
}

int main()
{
    stdio_init_all();
    sleep_ms(2000); // Aguarda a inicialização da porta serial

    inicia_pinos();

    aht20_reset(I2C_PORT);
    aht20_init(I2C_PORT);
    // Inicialização do Wi-Fi
    printf("Inicializando Wi-Fi...\n");
    if (cyw43_arch_init()) {
        printf("ERRO: Falha ao inicializar Wi-Fi\n");
        return -1;
    }
    
    cyw43_arch_enable_sta_mode();
    
    printf("Conectando ao Wi-Fi '%s'...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("ERRO: Falha ao conectar ao Wi-Fi\n");
        return -1;
    }
    
    printf("Wi-Fi conectado com sucesso!\n");
    printf("IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
    
    // Configuração do cliente MQTT
    static MQTT_CLIENT_DATA_T mqtt_state = {0};
    
    // Resolução DNS do servidor MQTT
    printf("Resolvendo DNS do servidor MQTT...\n");
    err_t err = dns_gethostbyname(MQTT_SERVER, &mqtt_state.mqtt_server_address, dns_found, &mqtt_state);
    if (err == ERR_OK) {
        dns_found(MQTT_SERVER, &mqtt_state.mqtt_server_address, &mqtt_state);
    } else if (err != ERR_INPROGRESS) {
        printf("ERRO: Falha na resolução DNS\n");
        return -1;
    }
    
    // Loop principal
    printf("Iniciando loop principal...\n");
    

    char str_lux[16];
    while (true) {
        lux = bh1750_read_measurement(I2C_PORT_BH);
        snprintf(str_lux, sizeof(str_lux), "%u", lux);
        printf("Luminosidade: %s lx\n", str_lux);

        if (aht20_read(I2C_PORT, &data))
        {
            float temp_com_offset = data.temperature + temp_offset;
            float umid_com_offset = data.humidity + umid_offset;
            // Lógica com histerese
            if (!relay_state && lux < (LUX_THRESHOLD - HYSTERESIS)) {
                // condição para ligar
                gpio_put(RELAY_PIN, 1); // Liga o relé
                relay_state = true;
                printf("Relé: LIGADO (lux %u < %d)\n", lux, LUX_THRESHOLD - HYSTERESIS);
            } else if (relay_state && lux > (LUX_THRESHOLD + HYSTERESIS)) {
                // condição para desligar
                gpio_put(RELAY_PIN, 0); // Desliga o relé
                relay_state = false;
                printf("Relé: DESLIGADO (lux %u > %d)\n", lux, LUX_THRESHOLD + HYSTERESIS);
            } else {
                // do nothing, mantém estado
            }
            printf("Temperatura AHT: %.2f (Corrigido: %.2f) C\n", data.temperature, temp_com_offset);
            printf("Umidade: %.2f (Corrigido: %.2f) %%\n\n\n", data.humidity, umid_com_offset);
        }
        else
        {
            printf("Erro na leitura do AHT10!\n\n\n");
        }
                // Processa eventos de rede
        cyw43_arch_poll();
        // Pequeno delay para não sobrecarregar o sistema
        sleep_ms(100);

    }
    cyw43_arch_deinit();
    return 0;
}