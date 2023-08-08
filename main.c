/******************************************************************************
* File Name: main.c
*
* Description: This is the source code for the PSoC 4 MSCLP self-capacitance
* button tuning code example for ModusToolbox.
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

/* Enable this, if Serial LED needs to be enabled */
#define ENABLE_SPI_SERIAL_LED           (1u)

/* Enable this, if Tuner needs to be enabled */
#define ENABLE_TUNER                    (1u)

/*******************************************************************************
* Fixed Macros
*******************************************************************************/
#define CAPSENSE_MSC0_INTR_PRIORITY     (3u)
#define CY_ASSERT_FAILED                (0u)

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


#if ENABLE_SPI_SERIAL_LED
void led_control();
#endif

#if CY_CAPSENSE_BIST_EN
static void measure_sensor_capacitance(uint32_t *sensor_capacitance);
#endif /* CY_CAPSENSE_BIST_EN */


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


    #if CY_CAPSENSE_BIST_EN
    /* Measure the self capacitance of sensor electrode using BIST */
    measure_sensor_capacitance(sensor_capacitance);
    #endif /* CY_CAPSENSE_BIST_EN */


    /* Start the first scan */
    Cy_CapSense_ScanAllSlots(&cy_capsense_context);

    for (;;)
    {
        if(CY_CAPSENSE_NOT_BUSY == Cy_CapSense_IsBusy(&cy_capsense_context))
        {
            /* Process all widgets */
            Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);


            #if ENABLE_SPI_SERIAL_LED
         /* Serial LED control for showing the CAPSENSE touch status (feedback) */
            led_control();
            #endif

            #if ENABLE_TUNER
            /* Establishes synchronized communication with the CAPSENSE Tuner tool */
            Cy_CapSense_RunTuner(&cy_capsense_context);
            #endif

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


#if ENABLE_SPI_SERIAL_LED
/*******************************************************************************
* Function Name: led_control
* ********************************************************************************
* Summary:
* Logic to control the on / off status of LED1 with green color
* based on the touch status of the CAPSENSE button widget.
*
*******************************************************************************/
void led_control()
{
    /* Brightness of each LED is represented by 0 to 255,
    *  where 0 indicates LED in OFF state and 255 indicate maximum
    *  brightness of an LED
    */
    volatile uint8_t brightness_max = 255u;
    volatile uint8_t brightness_min = 0u;

/*******************************************************************************
* If the CSD button is active, Turn On LED1 with color Green (Fixed intensity)
* ******************************************************************************/
    if (SENSOR_ACTIVE == Cy_CapSense_IsWidgetActive(CY_CAPSENSE_BUTTON_SELF_CAP_WDGT_ID, &cy_capsense_context))
    {
        led_context.led_num[LED1].color_red = brightness_min;
        led_context.led_num[LED1].color_green = brightness_max;
        led_context.led_num[LED1].color_blue = brightness_min;
    }
    else
    {
        led_context.led_num[LED1].color_red = brightness_min;
        led_context.led_num[LED1].color_green = brightness_min;
        led_context.led_num[LED1].color_blue = brightness_min;
    }

/*******************************************************************************
* Default, all the LEDs are turned off
********************************************************************************/
    led_context.led_num[LED2].color_red = brightness_min;
    led_context.led_num[LED2].color_green = brightness_min;
    led_context.led_num[LED2].color_blue = brightness_min;

    led_context.led_num[LED3].color_red = brightness_min;
    led_context.led_num[LED3].color_green = brightness_min;
    led_context.led_num[LED3].color_blue = brightness_min;

    serial_led_control(&led_context);
}
#endif


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
#endif /* CY_CAPSENSE_BIST_EN */


/* [] END OF FILE */
