/******************************************************************************
* File Name: main.c
*
* Description: This is the source code for the PSoC 4 MSCLP self-capacitance
* button tuning with Gesture detection code example for ModusToolbox.
*
* Related Document: See README.md
*
*******************************************************************************
* Copyright 2021-2023, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

/*******************************************************************************
 * Include header files
 ******************************************************************************/
#include "cy_pdl.h"
#include "cybsp.h"
#include "cycfg.h"
#include "cycfg_capsense.h"

#include "LEDcontrol.h"
#include "SpiMaster.h"

/*******************************************************************************
* User configurable Macros
********************************************************************************/

#define TIMESTAMP_INTERVAL_IN_MILSEC    (50u)
#define LED_TIMEOUT_IN_MILSEC           (1000u)

/*******************************************************************************
* Fixed Macros
*******************************************************************************/
#define CAPSENSE_MSC0_INTR_PRIORITY     (3u)
#define CY_ASSERT_FAILED                (0u)

#define ILO_FREQ                        (40000u)
#define TIME_IN_US                      (1000000u)

#define TIME_PER_TICK_IN_US             ((float)1/ILO_FREQ)*TIME_IN_US
#define SYS_TICK_INTERVAL               (TIMESTAMP_INTERVAL_IN_MILSEC*1000/(TIME_PER_TICK_IN_US))

#define ONE_FNGR_SINGLE_CLICK_GESTURE   (CY_CAPSENSE_GESTURE_ONE_FNGR_SINGLE_CLICK_MASK)
#define ONE_FNGR_DOUBLE_CLICK_GESTURE   (CY_CAPSENSE_GESTURE_ONE_FNGR_DOUBLE_CLICK_MASK)

#define TOUCHDOWN_GESTURE               (CY_CAPSENSE_GESTURE_TOUCHDOWN_MASK)
#define LIFTOFF_GESTURE                 (CY_CAPSENSE_GESTURE_LIFTOFF_MASK)
#define NO_GESTURE_DETECTED             (0u)

#define MAX_COUNTER_VALUE               (0xFFFFFFFF)


/* EZI2C interrupt priority must be higher than CAPSENSE&trade; interrupt. */
#define EZI2C_INTR_PRIORITY             (2u)

/* Define the conditions to check sensor status */
#define SENSOR_ACTIVE                   (1u)

/*******************************************************************************
* Global Variables
*******************************************************************************/
cy_stc_scb_ezi2c_context_t ezi2c_context;
stc_serial_led_context_t led_context;

/*******************************************************************************
* Function Prototypes
*******************************************************************************/
static void initialize_capsense(void);
static void capsense_msc0_isr(void);

static void ezi2c_isr(void);
static void initialize_capsense_tuner(void);

void led_control(void);

#if CY_CAPSENSE_BIST_EN
static void measure_sensor_capacitance(uint32_t *sensor_capacitance);
#endif 

static void init_sys_tick();
void SysTickCallback(void);

uint32_t gesture;
uint32_t gestureHeldForLed;
uint32_t led_delay;
uint32_t clickIntervalTimer;
uint8_t startDoubleClickTimer;
uint32_t doubleClickTimeout;

/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
*  System entrance point. This function performs
*  - initial setup of device
*  - initialize CAPSENSE
*  - initialize tuner communication
*  - scan touch input continuously
*  - serial RGB LED for touch indication
*
* Return:
*  int
*
*******************************************************************************/

int main(void)
{
    cy_rslt_t result;

    #if CY_CAPSENSE_BIST_EN
    uint32_t sensor_capacitance[CY_CAPSENSE_SENSOR_COUNT];
    #endif

    /* Initialize the device and board peripherals */
    result = cybsp_init();

    /* Board init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(CY_ASSERT_FAILED);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize SPI master */
    result = init_spi_master();

    /* Initialize EZI2C */
    initialize_capsense_tuner();

    /* Initialize MSC CAPSENSE */
    initialize_capsense();

    /* Initializes the system tick */
     init_sys_tick();

    #if CY_CAPSENSE_BIST_EN
    /* Measure the self capacitance of sensor electrode using BIST */
    measure_sensor_capacitance(sensor_capacitance);
    #endif 

    /* Start the first scan */
    Cy_CapSense_ScanAllSlots(&cy_capsense_context);

    /* Double click wait timeout before confirming single click detection */
    doubleClickTimeout = CY_CAPSENSE_BUTTON_SELF_CAP_CLICK_TIMEOUT_MAX_VALUE + CY_CAPSENSE_BUTTON_SELF_CAP_SECOND_CLICK_INTERVAL_MIN_VALUE;

    for (;;)
    {
        if(CY_CAPSENSE_NOT_BUSY == Cy_CapSense_IsBusy(&cy_capsense_context))
        {

             /* Process all widgets */
            Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);

            /*decode all the gestures*/
            gesture = Cy_CapSense_DecodeWidgetGestures(CY_CAPSENSE_BUTTON_SELF_CAP_WDGT_ID, &cy_capsense_context);

            /*Double click detection. Confirming single click only after double click detection timeout */
            if((gesture == ONE_FNGR_SINGLE_CLICK_GESTURE)&&(startDoubleClickTimer == 0))
            {
                clickIntervalTimer = 0u;
                startDoubleClickTimer = 1u;
            }
            else if((clickIntervalTimer > doubleClickTimeout)&&(startDoubleClickTimer))
            {
                clickIntervalTimer = 0u;
                startDoubleClickTimer = 0u;
                if(gesture == NO_GESTURE_DETECTED)
                {
                    gestureHeldForLed = ONE_FNGR_SINGLE_CLICK_GESTURE;
                    led_delay = 0;
                }
                else
                {
                    gestureHeldForLed = gesture;
                }
            }
            else if (( gesture != NO_GESTURE_DETECTED) && ( ( gesture != LIFTOFF_GESTURE ) && ( gesture != TOUCHDOWN_GESTURE ) ) )
            {
                clickIntervalTimer = 0u;
                startDoubleClickTimer = 0u;
                gestureHeldForLed = gesture;
                led_delay = 0;
            }

            else if ( ( gesture == NO_GESTURE_DETECTED )&& ( led_delay >= LED_TIMEOUT_IN_MILSEC  ) )
            {
                gestureHeldForLed = 0;
                led_delay = 0;
            }

            /* Serial LED control for showing the CAPSENSE touch and gesture detected status (feedback) */
            led_control();

            /* Establishes synchronized communication with the CAPSENSE Tuner tool */
            Cy_CapSense_RunTuner(&cy_capsense_context);

            /* Start the next scan */
            Cy_CapSense_ScanAllSlots(&cy_capsense_context);
        }
    }
}


/*******************************************************************************
* Function Name: initialize_capsense
********************************************************************************
* Summary:
*  This function initializes the CAPSENSE and configures the CAPSENSE
*  interrupt.
*
*******************************************************************************/
static void initialize_capsense(void)
{
    cy_capsense_status_t status = CY_CAPSENSE_STATUS_SUCCESS;

    /* CAPSENSE interrupt configuration MSCLP 0 */
    const cy_stc_sysint_t capsense_msc0_interrupt_config =
    {
        .intrSrc = CY_MSCLP0_LP_IRQ,
        .intrPriority = CAPSENSE_MSC0_INTR_PRIORITY,
    };

    /* Capture the MSC HW block and initialize it to the default state. */
    status = Cy_CapSense_Init(&cy_capsense_context);

    if (CY_CAPSENSE_STATUS_SUCCESS == status)
    {
        /* Initialize CAPSENSE interrupt for MSCLP 0 */
        Cy_SysInt_Init(&capsense_msc0_interrupt_config, capsense_msc0_isr);
        NVIC_ClearPendingIRQ(capsense_msc0_interrupt_config.intrSrc);
        NVIC_EnableIRQ(capsense_msc0_interrupt_config.intrSrc);

        /* Initialize the CAPSENSE firmware modules. */
        status = Cy_CapSense_Enable(&cy_capsense_context);
    }

    if(status != CY_CAPSENSE_STATUS_SUCCESS)
    {
        /* This status could fail before tuning the sensors correctly.
         * Ensure that this function passes after the CAPSENSE sensors are tuned
         * as per procedure give in the Readme.md file */
    }
}

/*******************************************************************************
* Function Name: capsense_msc0_isr
********************************************************************************
* Summary:
*  Wrapper function for handling interrupts from CAPSENSE MSC0 block.
*
*******************************************************************************/
static void capsense_msc0_isr(void)
{
    Cy_CapSense_InterruptHandler(CY_MSCLP0_HW, &cy_capsense_context);
}

/*******************************************************************************
* Function Name: initialize_capsense_tuner
********************************************************************************
* Summary:
* EZI2C module to communicate with the CAPSENSE Tuner tool.
*
*******************************************************************************/
static void initialize_capsense_tuner(void)
{
    cy_en_scb_ezi2c_status_t status = CY_SCB_EZI2C_SUCCESS;

    /* EZI2C interrupt configuration structure */
    const cy_stc_sysint_t ezi2c_intr_config =
    {
        .intrSrc = CYBSP_EZI2C_IRQ,
        .intrPriority = EZI2C_INTR_PRIORITY,
    };
    
    /* Initialize the EzI2C firmware module */
    status = Cy_SCB_EZI2C_Init(CYBSP_EZI2C_HW, &CYBSP_EZI2C_config, &ezi2c_context);
    if(status != CY_SCB_EZI2C_SUCCESS)
    {
        CY_ASSERT(CY_ASSERT_FAILED);
    }
    Cy_SysInt_Init(&ezi2c_intr_config, ezi2c_isr);
    NVIC_EnableIRQ(ezi2c_intr_config.intrSrc);
    
    /* Set the CAPSENSE data structure as the I2C buffer to be exposed to the
     * master on primary slave address interface. Any I2C host tools such as
     * the Tuner or the Bridge Control Panel can read this buffer but you can
     * connect only one tool at a time.
     */
    Cy_SCB_EZI2C_SetBuffer1(CYBSP_EZI2C_HW, (uint8_t *)&cy_capsense_tuner,
                            sizeof(cy_capsense_tuner), sizeof(cy_capsense_tuner),
                            &ezi2c_context);

    Cy_SCB_EZI2C_Enable(CYBSP_EZI2C_HW);
}

/*******************************************************************************
* Function Name: ezi2c_isr
********************************************************************************
* Summary:
* Wrapper function for handling interrupts from EZI2C block.
*
*******************************************************************************/
static void ezi2c_isr(void)
{
    Cy_SCB_EZI2C_Interrupt(CYBSP_EZI2C_HW, &ezi2c_context);
}

/*******************************************************************************
* Function Name: led_control
* ********************************************************************************
* Summary:
* Logic to control the on / off status of LED1 with green color
* based on the touch status of the CAPSENSE button widget.
*
*******************************************************************************/
void led_control(void)
{
    /* Brightness of each LED is represented by 0 to 255,
    *  where 0 indicates LED in OFF state and 255 indicate maximum
    *  brightness of an LED
    */
     uint8_t brightness_max = 255u;

    /*******************************************************************************
    * If there is no touch or gesture detected ,LED1 is off
    * ******************************************************************************/
    led_context.led_num[LED1].color_red = 0;
    led_context.led_num[LED1].color_green = 0;
    led_context.led_num[LED1].color_blue = 0;

    led_context.led_num[LED2].color_red = 0;
    led_context.led_num[LED2].color_green = 0;
    led_context.led_num[LED2].color_blue = 0;

    switch(gestureHeldForLed)
    {
        case ONE_FNGR_SINGLE_CLICK_GESTURE:
        /* If one finger single click gesture is performed, LED2 will glow RED */
            led_context.led_num[LED2].color_red = brightness_max;
            break;

        case ONE_FNGR_DOUBLE_CLICK_GESTURE:
         /* If one finger double click gesture is performed, LED2 will glow BLUE */
            led_context.led_num[LED2].color_blue = brightness_max;
            break;

        default:
        /*******************************************************************************
        * If the CSD button is active, Turn On LED1 with color Green (Fixed intensity)
        *******************************************************************************/
             if (SENSOR_ACTIVE == Cy_CapSense_IsWidgetActive(CY_CAPSENSE_BUTTON_SELF_CAP_WDGT_ID, &cy_capsense_context))
             {
              led_context.led_num[LED1].color_green = brightness_max;
             }
             break;
     }

    serial_led_control(&led_context);
}

#if CY_CAPSENSE_BIST_EN
/*******************************************************************************
* Function Name: measure_sensor_capacitance
********************************************************************************
* Summary:
*  Measures the self capacitance of the sensor electrode (Cp) in Femto Farad and
*  stores its value in the variable sensor_capacitance.
*
*******************************************************************************/
static void measure_sensor_capacitance(uint32_t *sensor_capacitance)
{
    /* For BIST configuration Connecting all Inactive sensor connections (ISC) of CSD sensors to to shield*/
    Cy_CapSense_SetInactiveElectrodeState(CY_CAPSENSE_SNS_CONNECTION_SHIELD,
                                                  CY_CAPSENSE_BIST_CSD_GROUP, &cy_capsense_context);

    /*Runs the BIST to measure the sensor capacitance*/
    Cy_CapSense_RunSelfTest(CY_CAPSENSE_BIST_SNS_CAP_MASK,
                &cy_capsense_context);
        memcpy(sensor_capacitance,
                cy_capsense_context.ptrWdConfig->ptrSnsCapacitance,
                CY_CAPSENSE_SENSOR_COUNT * sizeof(uint32_t));

}
#endif 

/*******************************************************************************
* Function Name: init_sys_tick
********************************************************************************
* Summary:
*  initializes the system tick with highest possible value to start counting down.
*  specifying the timestamp increment value
*******************************************************************************/

 static void init_sys_tick()
 {
    Cy_SysTick_Init (CY_SYSTICK_CLOCK_SOURCE_CLK_LF,SYS_TICK_INTERVAL);
    cy_capsense_context.ptrCommonContext->timestampInterval = TIMESTAMP_INTERVAL_IN_MILSEC;
    Cy_SysTick_SetCallback(0u,SysTickCallback);
 }


/*******************************************************************************
* Function Name: SysTickCallback
********************************************************************************
* Summary:
* Wrapper function for incrementing gesture timestamp and handling LED on time  
*
*******************************************************************************/

void SysTickCallback(void)
{
    Cy_CapSense_IncrementGestureTimestamp(&cy_capsense_context);

    if((led_delay + TIMESTAMP_INTERVAL_IN_MILSEC) < MAX_COUNTER_VALUE)
    {
        led_delay += TIMESTAMP_INTERVAL_IN_MILSEC;
    }
    
    if(((clickIntervalTimer + TIMESTAMP_INTERVAL_IN_MILSEC) < MAX_COUNTER_VALUE)&&(startDoubleClickTimer))
    {
        clickIntervalTimer += TIMESTAMP_INTERVAL_IN_MILSEC;
    }
}


/* [] END OF FILE */
