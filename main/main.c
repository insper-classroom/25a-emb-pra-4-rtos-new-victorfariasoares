#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <queue.h>

#include "ssd1306.h"
#include "gfx.h"

#include "pico/stdlib.h"
#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/timer.h" 

// ==================== DEFINIÇÕES DOS PINOS ====================
const int TRIG_PIN = 5;
const int ECHO_PIN = 16;  // Caso deseje usar 15, ajuste conforme o hardware

// ==================== RECURSOS RTOS ====================
QueueHandle_t xQueueTime;       // Fila para eventos (timestamp e borda)
QueueHandle_t xQueueDistance;   // Fila para enviar a distância calculada (em cm)
SemaphoreHandle_t xSemaphoreTrigger; // Semáforo para sinalizar que o trigger foi disparado

// ==================== ESTRUTURA DO EVENTO ====================
typedef struct {
    bool is_rising;      // true = borda de subida; false = borda de descida
    uint64_t timestamp;  // Timestamp em µs (obtido com to_us_since_boot(get_absolute_time()))
} echo_event_t;

// ==================== CALLBACK DE INTERRUPÇÃO ====================
void echo_pin_callback(uint gpio, uint32_t events) {
    echo_event_t evt;
    evt.timestamp = to_us_since_boot(get_absolute_time());

    if (events & GPIO_IRQ_EDGE_RISE) {
        evt.is_rising = true;
    } else if (events & GPIO_IRQ_EDGE_FALL) {
        evt.is_rising = false;
    }

    // Envia o evento para a fila; estamos na ISR, portanto não usamos timeout
    xQueueSendFromISR(xQueueTime, &evt, NULL);
}

// ==================== TASK: TRIGGER ====================
void trigger_task(void *p) {
    gpio_init(TRIG_PIN);
    gpio_set_dir(TRIG_PIN, GPIO_OUT);
    gpio_put(TRIG_PIN, 0);

    while (1) {
        // Gera pulso de 10 µs
        gpio_put(TRIG_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_put(TRIG_PIN, 0);

        // Sinaliza que o trigger foi disparado
        xSemaphoreGive(xSemaphoreTrigger);

        // Aguarda 1 segundo até o próximo trigger
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void echo_task(void *p) {
    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);
    gpio_set_irq_enabled_with_callback(ECHO_PIN,
                                       GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
                                       true,
                                       echo_pin_callback);

    // Variáveis para armazenar o tempo da borda de subida
    absolute_time_t t_rising = 0;
    bool have_rising = false;

    while (1) {
        echo_event_t evt;
        if (xQueueReceive(xQueueTime, &evt, portMAX_DELAY) == pdTRUE) {
            if (evt.is_rising) {
                // Guarda o timestamp da borda de subida
                t_rising = evt.timestamp;
                have_rising = true;
            } else {
                // Se houve borda de subida previamente, calcula a diferença
                if (have_rising) {
                    absolute_time_t t_falling = evt.timestamp;
                    have_rising = false;
                    
                    // Calcula a diferença em µs usando absolute_time_diff_us()
                    uint64_t diff = absolute_time_diff_us(t_rising, t_falling);
                    // Converte para distância (cm): velocidade do som 0.0343 cm/us; divide por 2 pois é ida e volta
                    double distance = (diff * 0.0343) / 2.0;
                    
                    // Envia a distância para a fila
                    xQueueSend(xQueueDistance, &distance, 0);
                }
            }
        }
    }
}

void oled_task(void *p) {
    ssd1306_init();
    ssd1306_t disp;
    gfx_init(&disp, 128, 32);

    // Exibe mensagem inicial
    gfx_clear_buffer(&disp);
    gfx_draw_string(&disp, 0, 0, 1, "Iniciando...");
    gfx_show(&disp);

    while (1) {
        // Aguarda o semáforo indicando que o trigger foi disparado
        if (xSemaphoreTake(xSemaphoreTrigger, pdMS_TO_TICKS(100)) == pdTRUE) {
            double distance = 0.0;
            // Espera até 50 ms para receber a distância da fila
            if (xQueueReceive(xQueueDistance, &distance, pdMS_TO_TICKS(50))) {
                if (distance > 400){
                    gfx_clear_buffer(&disp);
                    gfx_draw_string(&disp, 0, 0, 1, "Falha ao medir Distancia");
                    gfx_show(&disp);

                } else {
                    gfx_clear_buffer(&disp);
                
                char buf[32];
                snprintf(buf, sizeof(buf), "Dist: %.2f cm", distance);
                gfx_draw_string(&disp, 0, 0, 1, buf);

                // Desenha uma barra que representa a distância (limitada a 100 px)
                int bar_length = (int)distance;
                if (bar_length > 400)
                    bar_length = 128;
                gfx_draw_line(&disp, 0, 16, bar_length, 16);

                gfx_show(&disp);
                }
                
            } else {
                // Se não receber a distância em 50 ms, exibe mensagem de falha
                gfx_clear_buffer(&disp);
                gfx_draw_string(&disp, 0, 0, 1, "Sensor Falhou!");
                gfx_show(&disp);
            }
        }
    }
}

int main() {
    stdio_init_all();
    printf("Iniciando Sistema com FreeRTOS...\n");

    xQueueTime = xQueueCreate(10, sizeof(echo_event_t));
    xQueueDistance = xQueueCreate(10, sizeof(double));
    xSemaphoreTrigger = xSemaphoreCreateBinary();

    xTaskCreate(trigger_task, "TRIGGER_TASK", 256, NULL, 1, NULL);
    xTaskCreate(echo_task,    "ECHO_TASK",    256, NULL, 1, NULL);
    xTaskCreate(oled_task,    "OLED_TASK",    256, NULL, 1, NULL);

    vTaskStartScheduler();

    while (true)
        ;
}
