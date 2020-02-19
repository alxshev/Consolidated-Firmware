/******************************************************************************
 * Includes
 ******************************************************************************/
#include "auto_generated/App_CanTx.h"
#include "auto_generated/App_CanRx.h"
#include "BoardSpecifics.h"
#include "SharedCan.h"
#include "SharedAssert.h"
#include "SharedFreeRTOS.h"

/******************************************************************************
 * Module Preprocessor Constants
 ******************************************************************************/
/** @brief Size of a message in the CAN TX queue */
#define CAN_TX_MSG_FIFO_ITEM_SIZE sizeof(struct CanMsg)

/**
 * @brief Number of messages in the CAN TX queue. This value should be increased
 *        if CAN TX overflow happens frequently.
 */
#define CAN_TX_MSG_FIFO_LENGTH 20

/** @brief Size of a message in the CAN RX queue */
#define CAN_RX_MSG_FIFO_ITEM_SIZE sizeof(struct CanMsg)

/**
 * @brief Number of messages in the CAN RX queue. This value should be increased
 *        if CAN RX overflow happens frequently.
 */
#define CAN_RX_MSG_FIFO_LENGTH 20

/******************************************************************************
 * Module Preprocessor Macros
 ******************************************************************************/
/** @brief Board-specific struct type for the CAN startup message payload */
#define BOARD_STARTUP_STRUCT(board) _BOARD_STARTUP_STRUCT(board)
#define _BOARD_STARTUP_STRUCT(board) struct CanMsgs_##board##_startup_t

/** @brief Board-specific function to transmit the CAN startup message */
#define ENQUEUE_TRANSMIT_BOARD_STARTUP(BOARD, ARG) \
    _ENQUEUE_BOARD_STARTUP(BOARD, ARG)
#define _ENQUEUE_BOARD_STARTUP(BOARD, ARG) \
    App_CanTx_EnqueueNonPeriodicMsg_##BOARD##_STARTUP(ARG)

/******************************************************************************
 * Module Typedefs
 ******************************************************************************/

/******************************************************************************
 * Module Variable Definitions
 ******************************************************************************/
/** @brief FIFO for buffering CAN TX messages */
static uint8_t
    can_tx_msg_fifo_storage[CAN_TX_MSG_FIFO_LENGTH * CAN_TX_MSG_FIFO_ITEM_SIZE];

static struct StaticQueue can_tx_msg_fifo = {
    .storage = &can_tx_msg_fifo_storage[0],
    .state   = { { 0 } },
    .handle  = NULL,
};

/** @brief FIFO for buffering CAN RX messages */
static uint8_t
    can_rx_msg_fifo_storage[CAN_TX_MSG_FIFO_LENGTH * CAN_TX_MSG_FIFO_ITEM_SIZE];

static struct StaticQueue can_rx_msg_fifo = {
    .storage = &can_rx_msg_fifo_storage[0],
    .state   = { { 0 } },
    .handle  = NULL,
};

/** @brief Handle used to interface with underlying CAN hardware */
static CAN_HandleTypeDef *sharedcan_hcan = NULL;

/** @brief Boolean flag indicating whether this library is initialized */
static bool sharedcan_initialized = false;

static struct StaticSemaphore CanTxBinarySemaphore = {
    .handle  = NULL,
    .storage = { { 0 } },
};

/******************************************************************************
 * Private Function Prototypes
 ******************************************************************************/
/**
 * @brief  Initialize one or more CAN filters using 16-bit Filter Scale and
 *         Identifier Mask Mode (FSCx = 0, FBMx = 0)
 * @param  hcan Pointer to a CAN_HandleTypeDef structure that contains
 *         the configuration information for the specified CAN.
 * @param  filters Array of CAN filters.
 * @param  num_of_filters The number of CAN filters in the array.
 * @return ERROR: One or more filters didn't initialize properly
 *         SUCCESS: All filters initialized with no errors
 */
static ErrorStatus Io_InitializeFilters(
    CAN_HandleTypeDef *        hcan,
    CanMaskFilterConfig_Struct filters[],
    uint32_t                   num_of_filters);

/**
 * @brief Transmits a CAN message
 * @param message CAN message to transmit
 */
static HAL_StatusTypeDef Io_TransmitCanMessage(struct CanMsg *message);

/**
 * @brief  Shared callback function to be used in each RX FIFO callback
 *         (STM32F302x8's bxCAN peripheral has two - FIFO0 and FIFO1). We push
 *         the RX messages onto a queue which is intended to be consumed in user
 *         tasks.
 * @param  hcan Pointer to a CAN_HandleTypeDef structure that contains
 *         the configuration information for the specified CAN.
 * @param  rx_fifo The Rx FIFO that triggered the callback
 */
static void Io_CanRxCallback(CAN_HandleTypeDef *hcan, uint32_t rx_fifo);

/******************************************************************************
 * Private Function Definitions
 ******************************************************************************/
static ErrorStatus Io_InitializeFilters(
    CAN_HandleTypeDef *        hcan,
    CanMaskFilterConfig_Struct filters[],
    uint32_t                   num_of_filters)
{
    shared_assert(hcan != NULL);
    shared_assert(filters != NULL);

    static uint32_t filter_bank = 0;
    static uint32_t fifo        = CAN_FILTER_FIFO0;
    uint32_t        is_odd      = num_of_filters % 2;

    CAN_FilterTypeDef can_filter;
    can_filter.FilterMode       = CAN_FILTERMODE_IDMASK;
    can_filter.FilterScale      = CAN_FILTERSCALE_16BIT;
    can_filter.FilterActivation = CAN_FILTER_ENABLE;

    // Initialize two 16-bit filters for each filter bank
    for (uint32_t i = 0; i < num_of_filters / 2; i++)
    {
        // Configure filter settings
        can_filter.FilterIdLow          = filters[i].id;
        can_filter.FilterMaskIdLow      = filters[i].mask;
        can_filter.FilterIdHigh         = filters[i + 1].id;
        can_filter.FilterMaskIdHigh     = filters[i + 1].mask;
        can_filter.FilterFIFOAssignment = fifo;
        can_filter.FilterBank           = filter_bank;

        // Alternate between the two FIFOs
        fifo = !fifo;

        // Update filter bank for next iteration
        filter_bank = filter_bank + 1;

        // Configure and initialize filter bank
        if (HAL_CAN_ConfigFilter(hcan, &can_filter) != HAL_OK)
        {
            return ERROR;
        }
    }

    // For the odd-one-out filter, initialize two identical 16-bit filters
    // because each filter bank requires two 16-bit filters
    if (is_odd)
    {
        // Configure filter settings
        uint32_t last_filter_index      = num_of_filters - 1;
        can_filter.FilterIdLow          = filters[last_filter_index].id;
        can_filter.FilterMaskIdLow      = filters[last_filter_index].mask;
        can_filter.FilterIdHigh         = filters[last_filter_index].id;
        can_filter.FilterMaskIdHigh     = filters[last_filter_index].mask;
        can_filter.FilterFIFOAssignment = fifo;
        can_filter.FilterBank           = filter_bank;

        // Configure and initialize filter bank
        if (HAL_CAN_ConfigFilter(hcan, &can_filter) != HAL_OK)
        {
            return ERROR;
        }
    }

    return SUCCESS;
}

static HAL_StatusTypeDef Io_TransmitCanMessage(struct CanMsg *message)
{
    shared_assert(sharedcan_initialized == true);
    shared_assert(message != NULL);

    // Indicates the mailbox used for transmission, not currently used
    uint32_t mailbox = 0;

    CAN_TxHeaderTypeDef tx_header;

    tx_header.DLC   = message->dlc;
    tx_header.StdId = message->std_id;

    // The standard 11-bit CAN identifier is more than sufficient, so we disable
    // Extended CAN IDs by setting this field to zero.
    tx_header.ExtId = CAN_ExtID_NULL;

    // This field can be either Standard CAN or Extended CAN. See .ExtID to see
    // why we don't want Extended CAN
    tx_header.IDE = CAN_ID_STD;

    // This field can be either Data Frame or Remote Frame. For our
    // purpose, we only ever transmit Data Frames.
    tx_header.RTR = CAN_RTR_DATA;

    // Enabling this gives us a tick-based timestamp which we do not need. Plus,
    // it would take up 2 bytes of the CAN payload. So we disable the timestamp.
    tx_header.TransmitGlobalTime = DISABLE;

    return HAL_CAN_AddTxMessage(
        sharedcan_hcan, &tx_header, message->data, &mailbox);
}

static inline void Io_CanRxCallback(CAN_HandleTypeDef *hcan, uint32_t rx_fifo)
{
    shared_assert(sharedcan_initialized == true);
    shared_assert(hcan != NULL);

    // Track how many times the CAN TX FIFO has overflowed
    static uint32_t canrx_overflow_count = { 0 };

    BaseType_t          xHigherPriorityTaskWoken = pdFALSE;
    CAN_RxHeaderTypeDef header;
    struct CanMsg       message;

    if (HAL_CAN_GetRxMessage(hcan, rx_fifo, &header, &message.data[0]) ==
        HAL_OK)
    {
        // Copy metadata from HAL's CAN message struct into our custom CAN
        // message struct
        message.std_id = header.StdId;
        message.dlc    = header.DLC;

        // We defer reading the CAN RX message to a task by storing the message
        // on the CAN RX queue
        if (xQueueSendToBackFromISR(
                can_rx_msg_fifo.handle, &message, &xHigherPriorityTaskWoken) !=
            pdPASS)
        {
            // If the RX FIFO is full, we discard the message and log the
            // overflow over CAN.
            canrx_overflow_count++;
            App_CanTx_SetPeriodicSignal_RX_OVERFLOW_COUNT(canrx_overflow_count);
        }

        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/******************************************************************************
 * Function Definitions
 ******************************************************************************/
void SharedCan_Init(
    CAN_HandleTypeDef *        hcan,
    CanMaskFilterConfig_Struct filters[],
    uint32_t                   num_of_filters)
{
    shared_assert(hcan != NULL);
    shared_assert(filters != NULL);

    // Initialize CAN TX software queue
    can_tx_msg_fifo.handle = xQueueCreateStatic(
        CAN_TX_MSG_FIFO_LENGTH, CAN_TX_MSG_FIFO_ITEM_SIZE,
        can_tx_msg_fifo.storage, &can_tx_msg_fifo.state);
    shared_assert(can_tx_msg_fifo.handle != NULL);

    // Initialize binary semaphore for CAN TX task
    CanTxBinarySemaphore.handle =
        xSemaphoreCreateBinaryStatic(&CanTxBinarySemaphore.storage);
    shared_assert(CanTxBinarySemaphore.handle);

    // Initialize CAN RX software queue
    can_rx_msg_fifo.handle = xQueueCreateStatic(
        CAN_RX_MSG_FIFO_LENGTH, CAN_RX_MSG_FIFO_ITEM_SIZE,
        can_rx_msg_fifo.storage, &can_rx_msg_fifo.state);
    shared_assert(can_rx_msg_fifo.handle != NULL);

    // Initialize CAN hardware filters
    shared_assert(
        Io_InitializeFilters(hcan, filters, num_of_filters) == SUCCESS);

    // Configure interrupt mode for CAN peripheral
    shared_assert(
        HAL_CAN_ActivateNotification(
            hcan, CAN_IT_TX_MAILBOX_EMPTY | CAN_IT_RX_FIFO0_MSG_PENDING |
                      CAN_IT_RX_FIFO1_MSG_PENDING) == HAL_OK);

    // Enable the CAN peripheral
    shared_assert(HAL_CAN_Start(hcan) == HAL_OK);

    // Save a copy of the CAN handle that shall be used in this library
    sharedcan_hcan = hcan;

    // Set a boolean to indicate that the library is ready be used
    sharedcan_initialized = true;

    // Broadcast a CAN message to indicate that the system has rebooted
    BOARD_STARTUP_STRUCT(BOARD_NAME_LOWERCASE) pcb_startup = { .dummy = 0 };
    ENQUEUE_TRANSMIT_BOARD_STARTUP(BOARD_NAME_UPPERCASE, &pcb_startup);
}

void App_SharedCan_TxMessageQueueSendtoBack(struct CanMsg *message)
{
    shared_assert(sharedcan_initialized == true);
    shared_assert(message != NULL);

    // Track how many times the CAN TX FIFO has overflowed
    static uint32_t cantx_overflow_count = { 0 };

    if (xPortIsInsideInterrupt())
    {
        if (xQueueSendToBackFromISR(can_tx_msg_fifo.handle, message, NULL) !=
            pdTRUE)
        {
            // If the TX FIFO is full, we discard the message and log the
            // overflow over CAN.
            cantx_overflow_count++;
            App_CanTx_SetPeriodicSignal_TX_OVERFLOW_COUNT(cantx_overflow_count);
        }
        else if (
            uxQueueMessagesWaitingFromISR(CanTxBinarySemaphore.handle) == 0U)
        {
            // Give the binary semaphore only if it's not already given, or else
            // xSemaphoreGive() would fail and clutter up Tracealyzer.
            xSemaphoreGiveFromISR(CanTxBinarySemaphore.handle, NULL);
        }
    }
    else
    {
        if (xQueueSendToBack(can_tx_msg_fifo.handle, message, 0) != pdTRUE)
        {
            // If the TX FIFO is full, we discard the message and log the
            // overflow over CAN.
            cantx_overflow_count++;
            App_CanTx_SetPeriodicSignal_TX_OVERFLOW_COUNT(cantx_overflow_count);
        }
        // Without this if-statement, xSemaphore could fail and this would
        // clutter up the Tracealyzer trace.
        else if (uxQueueMessagesWaiting(CanTxBinarySemaphore.handle) == 0U)
        {
            // Give the binary semaphore only if it's not already given, or else
            // xSemaphoreGive() would fail and clutter up Tracealyzer.
            xSemaphoreGive(CanTxBinarySemaphore.handle);
        }
    }
}

void App_SharedCan_ReadRxMessagesIntoTableFromTask(void)
{
    shared_assert(sharedcan_initialized == true);

    struct CanMsg message;

    // Get a message from the RX queue and process it, else block forever.
    if (xQueueReceive(can_rx_msg_fifo.handle, &message, portMAX_DELAY) ==
        pdTRUE)
    {
        App_CanRx_ReadMessageIntoTableFromTask(
            message.std_id, &message.data[0]);
    }
}

void App_SharedCan_TransmitEnqueuedCanTxMessagesFromTask(void)
{
    shared_assert(sharedcan_initialized == true);

    xSemaphoreTake(CanTxBinarySemaphore.handle, portMAX_DELAY);

    while (HAL_CAN_GetTxMailboxesFreeLevel(sharedcan_hcan) > 0 &&
           uxQueueMessagesWaiting(can_tx_msg_fifo.handle) > 0)
    {
        struct CanMsg message;
        if (xQueuePeek(can_tx_msg_fifo.handle, &message, 0) == pdTRUE)
        {
            (void)Io_TransmitCanMessage(&message);

            // Remove the message from CAN TX queue if we were able to transmit
            // it. This should never fail. If it ever does, then the CAN TX
            // queue is probably being consumed somewhere else by mistake.
            shared_assert(
                xQueueReceive(can_tx_msg_fifo.handle, &message, 0) == pdTRUE);
        }
    }
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    /* NOTE: All receive mailbox interrupts shall be handled in the same way */
    Io_CanRxCallback(hcan, CAN_RX_FIFO0);
}

void HAL_CAN_RxFifo1MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    /* NOTE: All receive mailbox interrupts shall be handled in the same way */
    Io_CanRxCallback(hcan, CAN_RX_FIFO1);
}

void HAL_CAN_TxMailbox0CompleteCallback(CAN_HandleTypeDef *hcan)
{
    /* NOTE: All transmit mailbox interrupts shall be handled in the same way */
    UNUSED(hcan);
    xSemaphoreGiveFromISR(CanTxBinarySemaphore.handle, NULL);
}

void HAL_CAN_TxMailbox1CompleteCallback(CAN_HandleTypeDef *hcan)
{
    /* NOTE: All transmit mailbox interrupts shall be handled in the same way */
    UNUSED(hcan);
    xSemaphoreGiveFromISR(CanTxBinarySemaphore.handle, NULL);
}

void HAL_CAN_TxMailbox2CompleteCallback(CAN_HandleTypeDef *hcan)
{
    /* NOTE: All transmit mailbox interrupts shall be handled in the same way */
    UNUSED(hcan);
    xSemaphoreGiveFromISR(CanTxBinarySemaphore.handle, NULL);
}
