/*
 * Copyright 2018 David B Brown (@maccoylton)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Example of using esp-homekit library to control
 * mutiple sensors
 * The use and OTA mechanis created by HomeACcessoryKid
 *
 */

#define DEVICE_MANUFACTURER "David B Brown"
#define DEVICE_NAME "Multi-Sensor"
#define DEVICE_MODEL "1"
#define DEVICE_SERIAL "12345678"
#define FW_VERSION "1.0"
#define LED_GPIO 13
#define MOTION_SENSOR_GPIO 5
#define TEMPERATURE_SENSOR_PIN 4
#define TEMPERATURE_POLL_PERIOD 10000

#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <espressif/esp_common.h>
#include <esp/uart.h>
#include <esplibs/libmain.h>
#include <esp8266.h>	
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <string.h>
#include <wifi_config.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <dht/dht.h>
#include <http_post.h>
#include <adv_button.h>
#include <led_codes.h>
#include <custom_characteristics.h>
#include <shared_functions.h>


const int RESET_BUTTON_GPIO = 0;

int led_off_value=0; /* global varibale to support LEDs set to 0 where the LED is connected to GND, 1 where +3.3v */
const int status_led_gpio = 13; /*set the gloabl variable for the led to be sued for showing status */


// add this section to make your device OTA capable
// create the extra characteristic &ota_trigger, at the end of the primary service (before the NULL)
// it can be used in Eve, which will show it, where Home does not
// and apply the four other parameters in the accessories_information section

#include <ota-api.h>

TaskHandle_t http_post_tasks_handle;
char accessory_name[64];

homekit_characteristic_t wifi_reset   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_RESET, false, .setter=wifi_reset_set);
homekit_characteristic_t wifi_check_interval   = HOMEKIT_CHARACTERISTIC_(CUSTOM_WIFI_CHECK_INTERVAL, 10, .setter=wifi_check_interval_set);
homekit_characteristic_t task_stats   = HOMEKIT_CHARACTERISTIC_(CUSTOM_TASK_STATS, false , .setter=task_stats_set);
homekit_characteristic_t ota_beta     = HOMEKIT_CHARACTERISTIC_(CUSTOM_OTA_BETA, false, .setter=ota_beta_set);
homekit_characteristic_t lcm_beta    = HOMEKIT_CHARACTERISTIC_(CUSTOM_LCM_BETA, false, .setter=lcm_beta_set);

homekit_characteristic_t ota_trigger      = API_OTA_TRIGGER;
homekit_characteristic_t name             = HOMEKIT_CHARACTERISTIC_(NAME, DEVICE_NAME);
homekit_characteristic_t manufacturer     = HOMEKIT_CHARACTERISTIC_(MANUFACTURER,  DEVICE_MANUFACTURER);
homekit_characteristic_t serial           = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, DEVICE_SERIAL);
homekit_characteristic_t model            = HOMEKIT_CHARACTERISTIC_(MODEL,         DEVICE_MODEL);
homekit_characteristic_t revision         = HOMEKIT_CHARACTERISTIC_(FIRMWARE_REVISION,  FW_VERSION);

//light sensor
homekit_characteristic_t currentAmbientLightLevel = HOMEKIT_CHARACTERISTIC_(CURRENT_AMBIENT_LIGHT_LEVEL, 0,.min_value = (float[]) {0},);
homekit_characteristic_t status_active    = HOMEKIT_CHARACTERISTIC_(STATUS_ACTIVE, 1);

//motion sensor
homekit_characteristic_t motion_detected  = HOMEKIT_CHARACTERISTIC_(MOTION_DETECTED, 0);

//temperature sensor
homekit_characteristic_t current_temperature = HOMEKIT_CHARACTERISTIC_( CURRENT_TEMPERATURE, 0 );

//humidity sensor
homekit_characteristic_t current_relative_humidity    = HOMEKIT_CHARACTERISTIC_( CURRENT_RELATIVE_HUMIDITY, 0 );



homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_switch, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            &name,
            &manufacturer,
            &serial,
            &model,
            &revision,
            HOMEKIT_CHARACTERISTIC(IDENTIFY, identify),
            NULL
        }),
        HOMEKIT_SERVICE(LIGHT_SENSOR, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Light Sensor"),
            &currentAmbientLightLevel,
            &status_active,
            &ota_trigger,
            &wifi_reset,
            &wifi_check_interval,
            &task_stats,
            &ota_beta,
            &lcm_beta,
            NULL
        }),
        HOMEKIT_SERVICE(MOTION_SENSOR, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Motion Sensor"),
            &motion_detected,
            NULL
        }),
        HOMEKIT_SERVICE(TEMPERATURE_SENSOR, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Temperature Sensor"),
            &current_temperature,
            NULL
        }),
        HOMEKIT_SERVICE(HUMIDITY_SENSOR, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Humidity Sensor"),
            &current_relative_humidity,
            NULL
        }),
        NULL
    }),
    NULL
};


void temperature_sensor_task(void *_args) {
    
    int loop_count = 10;
    
    float humidity_value, temperature_value;
    while (1) {
        bool success = dht_read_float_data(
                                           DHT_TYPE_DHT22, TEMPERATURE_SENSOR_PIN,
                                           &humidity_value, &temperature_value
                                           );
        
        if (success) {
            printf("%s: Got readings: temperature %g, humidity %g\n", __func__, temperature_value, humidity_value);
            current_temperature.value = HOMEKIT_FLOAT(temperature_value);
            current_relative_humidity.value = HOMEKIT_FLOAT(humidity_value);
            
            homekit_characteristic_notify(&current_temperature, current_temperature.value);
            homekit_characteristic_notify(&current_relative_humidity, current_relative_humidity.value);
            
            if (loop_count == 10) {
                snprintf (post_string, 150, "sql=insert into homekit.temperaturesensorlog (TemperatureSensorName, Temperature) values ('%s', %f)", accessory_name, temperature_value);
                printf ("%s: Post String: %s\n", __func__, post_string);
                vTaskResume( http_post_tasks_handle );
                loop_count = 0;
            }
            
            if (loop_count == 5) {
                snprintf (post_string, 150, "sql=insert into homekit.humiditysensorlog (HumiditySensorName, Humidity) values ('%s', %f)", accessory_name, humidity_value);
                printf ("%s: Post String: %s\n", __func__, post_string);
                vTaskResume( http_post_tasks_handle );
            }
            
        } else {
            led_code (LED_GPIO, SENSOR_ERROR);
            printf("%s: Couldnt read data from sensor DHT22\n", __func__);
        }
        vTaskDelay(TEMPERATURE_POLL_PERIOD / portTICK_PERIOD_MS);
        loop_count++;
    }
}

void temperature_sensor_init() {
    xTaskCreate(temperature_sensor_task, "Temperature", 256, NULL, tskIDLE_PRIORITY+1, NULL);
}


void motion_sensor_callback(uint8_t gpio) {
    
    
    if (gpio == MOTION_SENSOR_GPIO){
        int new = 0;
        new = gpio_read(MOTION_SENSOR_GPIO);
        motion_detected.value = HOMEKIT_BOOL(new);
        homekit_characteristic_notify(&motion_detected, HOMEKIT_BOOL(new));
        if (new == 1) {
            printf("%s: Motion Detected on %d\n", __func__, gpio);
            snprintf (post_string, 150, "sql=insert into homekit.motionsensorlog (MotionSensorName, MotionDetectionState) values ('%s', 1)", accessory_name);
            printf ("%s: Post String: %s\n", __func__,post_string);
            vTaskResume( http_post_tasks_handle );
        } else {
            printf("%s: Motion Stopped on %d\n", __func__,gpio);
            snprintf (post_string, 150, "sql=insert into homekit.motionsensorlog (MotionSensorName, MotionDetectionState) values ('%s', 0)", accessory_name);
            printf ("%s: Post String: %s\n", __func__,post_string);
            vTaskResume( http_post_tasks_handle );
        }
    }
    else {
        led_code (LED_GPIO, GENERIC_ERROR);
        printf("%s: Interrupt on %d", __func__,gpio);
    }
    
}

void motion_sensor_init() {
    
    gpio_set_interrupt(MOTION_SENSOR_GPIO, GPIO_INTTYPE_EDGE_ANY, motion_sensor_callback);
}

void light_sensor_task(void *_args) {
    
    // thaks to https://github.com/peros550/esp-homekit-multiple-sensors/blob/master/examples/multiple_sensors/multiple_sensors.c
    
    int loop_count = 60;
    
    uint16_t analog_light_value;
    while (1) {
        analog_light_value = sdk_system_adc_read();
        //The below code does not produce accurate LUX readings which is what homekit expects. It only provides an indication of brightness on a scale between 0 to 1024
        //More work needs to be done so that accurate conversation to LUX scale can take place. However this is strongly dependent on the type of sensor used.
        //In my case I used a Photodiode Light Sensor
        currentAmbientLightLevel.value.float_value = (1024 - analog_light_value);
        homekit_characteristic_notify(&currentAmbientLightLevel, HOMEKIT_FLOAT((1024 - analog_light_value)));
        printf ("%s: Light level: %i\n", __func__, (1024 - analog_light_value));
        if (loop_count == 10){
            snprintf (post_string, 150, "sql=insert into homekit.lightsensorlog (LightSensorName, LightLevel) values ('%s', %f)", accessory_name, currentAmbientLightLevel.value.float_value);
            printf ("%s: Post String: %s\n", __func__, post_string);
            vTaskResume( http_post_tasks_handle );
            loop_count = 0;
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        loop_count ++;
    }
}

void light_sensor_init() {
    xTaskCreate(light_sensor_task, "Light Sensor", 256, NULL, tskIDLE_PRIORITY+1, NULL);
}

void multi_sensor_init (){
 
    printf ("%s: Muti Sensor Init \n", __func__);

    xTaskCreate(http_post_task, "http post task", 512, NULL, tskIDLE_PRIORITY+1, &http_post_tasks_handle);
    light_sensor_init();
    motion_sensor_init();
    temperature_sensor_init();

}

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
    .on_event = on_homekit_event
};


void save_characteristics (  ) {
    /* called by a timer function to save charactersitics */
}


void recover_from_reset (int reason){
    /* called if we restarted abnormally */
    printf ("%s: reason %d\n", __func__, reason);
}


void accessory_init (void ){
    /* initalise anything you don't want started until wifi and pairing is confirmed */
    multi_sensor_init ();

}

void accessory_init_not_paired (void) {
    /* initalise anything you don't want started until wifi and homekit imitialisation is confirmed, but not paired */
}



void gpio_init (void){
    
    printf ("%s: GPIO Init\n", __func__);
    adv_button_create(RESET_BUTTON_GPIO, true, false);
    adv_button_register_callback_fn(RESET_BUTTON_GPIO, reset_button_callback, VERYLONGPRESS_TYPE, NULL, 0);

    gpio_enable(MOTION_SENSOR_GPIO, GPIO_INPUT);
    gpio_set_pullup(MOTION_SENSOR_GPIO, false, false);

    gpio_enable(LED_GPIO, GPIO_OUTPUT);
    gpio_write(LED_GPIO, false);
    
    gpio_enable(TEMPERATURE_SENSOR_PIN, GPIO_INPUT);
    gpio_set_pullup(TEMPERATURE_SENSOR_PIN, false, false);
    
}


void user_init(void) {

    
    standard_init (&name, &manufacturer, &model, &serial, &revision);

    gpio_init ();
    
    strcpy (accessory_name, name.value.string_value);

    wifi_config_init(DEVICE_NAME, NULL, on_wifi_ready);

    
}
