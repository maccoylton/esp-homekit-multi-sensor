/*
 * Example of using esp-homekit library to control
 * mutiple sensors
 * The use and OTA mechanis created by HomeACcessoryKid 
 *
 */

#define DEVICE_MANUFACTURER "David B Brown"
#define DEVICE_NAME "Muti-Sensor"
#define DEVICE_MODEL "1"
#define DEVICE_SERIAL "12345678"
#define FW_VERSION "1.0"
#define LED_GPIO 2
#define MOTION_SENSOR_GPIO 12
#define TEMPERATURE_SENSOR_PIN 4
#define TEMPERATURE_POLL_PERIOD 10000

#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <espressif/esp_common.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <string.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <dht/dht.h>
#include <http_post.h>
#include <button.h>
#include <led_codes.h>


const int button_gpio = 0;

// add this section to make your device OTA capable
// create the extra characteristic &ota_trigger, at the end of the primary service (before the NULL)
// it can be used in Eve, which will show it, where Home does not
// and apply the four other parameters in the accessories_information section

#include <ota-api.h>

TaskHandle_t http_post_tasks_handle;
char accessory_name[64];

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


void identify_task(void *_args) {

    led_code(LED_GPIO, IDENTIFY_ACCESSORY);
    vTaskDelete(NULL);
}

void identify(homekit_value_t _value) {
    printf("identify\n");
    xTaskCreate(identify_task, "identify", 128, NULL, 2, NULL);
}


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

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111"
};


void create_accessory_name() {

    int serialLength = snprintf(NULL, 0, "%d", sdk_system_get_chip_id());

    char *serialNumberValue = malloc(serialLength + 1);

    snprintf(serialNumberValue, serialLength + 1, "%d", sdk_system_get_chip_id());
    
    int name_len = snprintf(NULL, 0, "%s-%s-%s",
				DEVICE_NAME,
				DEVICE_MODEL,
				serialNumberValue);

    if (name_len > 63) {
        name_len = 63;
    }

    char *name_value = malloc(name_len + 1);

    snprintf(name_value, name_len + 1, "%s-%s-%s",
		 DEVICE_NAME, DEVICE_MODEL, serialNumberValue);

    strcpy (accessory_name, name_value);
   
    name.value = HOMEKIT_STRING(name_value);
    serial.value = name.value;
}


void reset_configuration_task() {

    led_code(LED_GPIO, WIFI_CONFIG_RESET);
    
//    printf("Resetting Wifi Config\n");
    
//    wifi_config_reset();
    
//    vTaskDelay(1000 / portTICK_PERIOD_MS);
    
    printf("Resetting HomeKit Config\n");
    
    homekit_server_reset();
    
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    
    printf("Restarting\n");
    
    sdk_system_restart();
    
    vTaskDelete(NULL);
}

void reset_configuration() {
    printf("Resetting Sonoff configuration\n");
    xTaskCreate(reset_configuration_task, "Reset configuration", 256, NULL, 2, NULL);
}

void button_callback(uint8_t gpio, button_event_t event) {
    switch (event) {
        case button_event_single_press:
	    printf("Button event: %d, doing nothin\n", event);
            break;
        case button_event_long_press:
	    printf("Button event: %d, resetting homekit config\n", event);
            reset_configuration();
            break;
        default:
            printf("Unknown button event: %d\n", event);
    }
}

void temperature_sensor_task(void *_args) {

    int loop_count = 10;
    gpio_set_pullup(TEMPERATURE_SENSOR_PIN, false, false);

    float humidity_value, temperature_value;
    while (1) {
        bool success = dht_read_float_data(
            DHT_TYPE_DHT22, TEMPERATURE_SENSOR_PIN,
            &humidity_value, &temperature_value
        );

        if (success) {
            printf("Got readings: temperature %g, humidity %g\n", temperature_value, humidity_value);
            current_temperature.value = HOMEKIT_FLOAT(temperature_value);
            current_relative_humidity.value = HOMEKIT_FLOAT(humidity_value);

            homekit_characteristic_notify(&current_temperature, current_temperature.value);
            homekit_characteristic_notify(&current_relative_humidity, current_relative_humidity.value);

	    if (loop_count == 10) {
	    	snprintf (post_string, 150, "sql=insert into homekit.temperaturesensorlog (TemperatureSensorName, Temperature) values ('%s', %f)", accessory_name, temperature_value);
	    	printf ("Post String: %s\n", post_string);
            	vTaskResume( http_post_tasks_handle );
	    	loop_count = 0;
	    }

            if (loop_count == 5) {
                snprintf (post_string, 150, "sql=insert into homekit.humiditysensorlog (HumiditySensorName, Humidity) values ('%s', %f)", accessory_name, humidity_value);
                printf ("Post String: %s\n", post_string);
                vTaskResume( http_post_tasks_handle );
            }
	
        } else {
	led_code (LED_GPIO, SENSOR_ERROR );
            printf("Couldnt read data from sensor\n");
        }
        vTaskDelay(TEMPERATURE_POLL_PERIOD / portTICK_PERIOD_MS);
	loop_count++;
    }
}

void temperature_sensor_init() {
    xTaskCreate(temperature_sensor_task, "Temperature", 256, NULL, 2, NULL);
}


void motion_sensor_callback(uint8_t gpio) {


    if (gpio == MOTION_SENSOR_GPIO){
        int new = 0;
        new = gpio_read(MOTION_SENSOR_GPIO);
        motion_detected.value = HOMEKIT_BOOL(new);
        homekit_characteristic_notify(&motion_detected, HOMEKIT_BOOL(new));
        if (new == 1) {
                printf("Motion Detected on %d\n", gpio);
                snprintf (post_string, 150, "sql=insert into homekit.motionsensorlog (MotionSensorName, MotionDetectionState) values ('%s', 1)", accessory_name);
            	printf ("Post String: %s\n", post_string);
                vTaskResume( http_post_tasks_handle );
        } else {
                printf("Motion Stopped on %d\n", gpio);
                snprintf (post_string, 150, "sql=insert into homekit.motionsensorlog (MotionSensorName, MotionDetectionState) values ('%s', 0)", accessory_name);
                printf ("Post String: %s\n", post_string);
                vTaskResume( http_post_tasks_handle );
        }
    }
    else {
        led_code (LED_GPIO, GENERIC_ERROR);
        printf("Interrupt on %d", gpio);
    }

}

void motion_sensor_init() {

    gpio_enable(MOTION_SENSOR_GPIO, GPIO_INPUT);
    gpio_set_pullup(MOTION_SENSOR_GPIO, false, false);
    gpio_set_interrupt(MOTION_SENSOR_GPIO, GPIO_INTTYPE_EDGE_ANY, motion_sensor_callback);
}

void light_sensor_task(void *_args) {

// thaks to https://github.com/peros550/esp-homekit-multiple-sensors/blob/master/examples/multiple_sensors/multiple_sensors.c

    int loop_count = 10;

    uint16_t analog_light_value;
    while (1) {
         analog_light_value = sdk_system_adc_read();
		 //The below code does not produce accurate LUX readings which is what homekit expects. It only provides an indication of brightness on a scale between 0 to 1024
		//More work needs to be done so that accurate conversation to LUX scale can take place. However this is strongly dependent on the type of sensor used. 
		//In my case I used a Photodiode Light Sensor 
            currentAmbientLightLevel.value.float_value = (1024 - analog_light_value);
            homekit_characteristic_notify(&currentAmbientLightLevel, HOMEKIT_FLOAT((1024 - analog_light_value)));
	    printf ("Light level: %i\n", (1024 - analog_light_value));
	    if (loop_count == 10){
            	snprintf (post_string, 150, "sql=insert into homekit.lightsensorlog (LightSensorName, LightLevel) values ('%s', %f)", accessory_name, currentAmbientLightLevel.value.float_value);
            	printf ("Post String: %s\n", post_string);
		vTaskResume( http_post_tasks_handle );
		loop_count = 0;
	    }
            vTaskDelay(30000 / portTICK_PERIOD_MS);
	    loop_count ++;
    }
}

void light_sensor_init() {
    xTaskCreate(light_sensor_task, "Light Sensor", 256, NULL, 2, NULL);
}


void user_init(void) {
    uart_set_baud(0, 115200);


    create_accessory_name(); 
    light_sensor_init();
    motion_sensor_init();
    temperature_sensor_init();
    gpio_enable(LED_GPIO, GPIO_OUTPUT);


    int c_hash=ota_read_sysparam(&manufacturer.value.string_value,&serial.value.string_value,
                                      &model.value.string_value,&revision.value.string_value);
    if (c_hash==0) c_hash=1;
        config.accessories[0]->config_number=c_hash;

    if (button_create(button_gpio, 0, 4000, button_callback)) {
        printf("Failed to initialize button\n");
    }

    homekit_server_init(&config);

    xTaskCreate(http_post_task, "http post task", 512, NULL, 2, &http_post_tasks_handle);

}
