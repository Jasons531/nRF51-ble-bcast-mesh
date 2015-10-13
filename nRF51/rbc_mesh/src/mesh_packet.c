/***********************************************************************************
Copyright (c) Nordic Semiconductor ASA
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

  3. Neither the name of Nordic Semiconductor ASA nor the names of other
  contributors to this software may be used to endorse or promote products
  derived from this software without specific prior written permission.

  4. This software must only be used in a processor manufactured by Nordic
  Semiconductor ASA, or in a processor manufactured by a third party that
  is used in combination with a processor manufactured by Nordic Semiconductor.


THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************************************************/

#include "mesh_packet.h"
#include "app_error.h"
#include <string.h>

/******************************************************************************
* Static globals
******************************************************************************/
static mesh_packet_t g_packet_pool[MESH_PACKET_POOL_SIZE];
static uint8_t g_packet_refs[MESH_PACKET_POOL_SIZE];
/******************************************************************************
* Interface functions
******************************************************************************/
void mesh_packet_init(void)
{
    for (uint32_t i = 0; i < MESH_PACKET_POOL_SIZE; ++i)
    {
        /* reset ref count field */
        g_packet_refs[i] = 0;
    }
}

void mesh_packet_on_ts_begin(void)
{
    /* do nothing */
}

bool mesh_packet_acquire(mesh_packet_t** pp_packet)
{
    for (uint32_t i = 0; i < MESH_PACKET_POOL_SIZE; ++i)
    {
        if (g_packet_refs[i] == 0) /* no refs, free to use */
        {
            g_packet_refs[i] = 1;
            *pp_packet = &g_packet_pool[i];
            return true;
        }
    }
    return false;
}

bool mesh_packet_ref_count_inc(mesh_packet_t* p_packet)
{
    uint32_t index = (((uint32_t) p_packet) - ((uint32_t) &g_packet_pool[0])) / sizeof(mesh_packet_t);
    if (index > MESH_PACKET_POOL_SIZE)
        return false;

    /* the given pointer may not be aligned, have to force alignment with index */
    if (++g_packet_refs[index] == 0x00) /* check for rollover */
    {
        APP_ERROR_CHECK(NRF_ERROR_NO_MEM);
    }
    return true;
}

bool mesh_packet_ref_count_dec(mesh_packet_t* p_packet)
{
    uint32_t index = (((uint32_t) p_packet) - ((uint32_t) &g_packet_pool[0])) / sizeof(mesh_packet_t);
    if (index > MESH_PACKET_POOL_SIZE)
        return false;
    
    /* make sure that we aren't rolling over the ref count */
    if (g_packet_refs[index] == 0x00)
    {
        return false;
    }
    g_packet_refs[index]--;

    return true;
}

uint32_t mesh_packet_set_local_addr(mesh_packet_t* p_packet)
{
#ifdef SOFTDEVICE_PRESENT
    ble_gap_addr_t my_addr;
    uint32_t error_code = sd_ble_gap_address_get(&my_addr);
    if (error_code != NRF_SUCCESS)
    {
        return error_code;
    }
    p_packet->header.addr_type = my_addr.addr_type;
    memcpy(p_packet->addr, my_addr.addr, BLE_GAP_ADDR_LEN);
#else
    memcpy(p_packet->addr, &NRF_FICR->DEVICEADDR[0], BLE_GAP_ADDR_LEN);
    p_packet->header.addr_type = NRF_FICR->DEVICEADDRTYPE;
#endif
    
    return NRF_SUCCESS;
}

uint32_t mesh_packet_build(mesh_packet_t* p_packet, 
        rbc_mesh_value_handle_t handle,
        uint16_t version,
        uint8_t* data,
        uint8_t length)
{
    if (p_packet == NULL)
    {
        return NRF_ERROR_NULL;
    }
    /* place mesh adv data at beginning of adv payload */
    mesh_adv_data_t* p_mesh_adv_data = (mesh_adv_data_t*) &p_packet->payload[0];

    if (length > RBC_MESH_VALUE_MAX_LEN)
    {
        return NRF_ERROR_INVALID_LENGTH;
    }
    
    mesh_packet_set_local_addr(p_packet);

    p_packet->header.length = MESH_PACKET_OVERHEAD + length;
    p_packet->header.type = BLE_PACKET_TYPE_ADV_NONCONN_IND;
    
    /* fill mesh adv data header fields */
    p_mesh_adv_data->adv_data_length = MESH_PACKET_ADV_OVERHEAD + length;
    p_mesh_adv_data->adv_data_type = MESH_ADV_DATA_TYPE;
    p_mesh_adv_data->mesh_uuid = MESH_UUID;
    
    p_mesh_adv_data->handle = handle;
    p_mesh_adv_data->version = version;
    if (length > 0 && data != NULL)
    {
        memcpy(p_mesh_adv_data->data, data, length);
    }

    return NRF_SUCCESS;
}

uint32_t mesh_packet_adv_data_sanitize(mesh_packet_t* p_packet)
{
    mesh_adv_data_t* p_mesh_adv_data = mesh_packet_adv_data_get(p_packet);
    if (p_mesh_adv_data == NULL)
    {
        return NRF_ERROR_INVALID_DATA;
    }
    if (((uint8_t*)p_mesh_adv_data) != &p_packet->payload[0])
    {
        /* must move the adv data to the beginning of the advertisement payload */
        const uint8_t adv_data_length = p_mesh_adv_data->adv_data_length + 1;
        for (uint32_t i = 0; i < adv_data_length; ++i)
        {
            /* memcpy is unsafe for overlapping memory, memmove is slower than 
               necessary -> move it manually. */
            p_packet->payload[i] = *(((uint8_t*) p_mesh_adv_data) + i);
        }
        p_mesh_adv_data = (mesh_adv_data_t*) &p_packet->payload[0];
    }

    /* only fit mesh adv data */
    p_packet->header.length = 
        MESH_PACKET_OVERHEAD - 
        MESH_PACKET_ADV_OVERHEAD + 
        p_mesh_adv_data->adv_data_length; 

    return NRF_SUCCESS;
}

mesh_adv_data_t* mesh_packet_adv_data_get(mesh_packet_t* p_packet)
{
    if (p_packet == NULL)
        return NULL;
    /* run through advertisement data to find mesh data */
    mesh_adv_data_t* p_mesh_adv_data = (mesh_adv_data_t*) &p_packet->payload[0];
    while (p_mesh_adv_data->adv_data_type != MESH_ADV_DATA_TYPE || 
            p_mesh_adv_data->mesh_uuid != MESH_UUID)
    {
        p_mesh_adv_data += p_mesh_adv_data->adv_data_length + 1;
        
        if (((uint8_t*) p_mesh_adv_data) >= &p_packet->payload[BLE_ADV_PACKET_PAYLOAD_MAX_LENGTH])
        {
            /* couldn't find mesh data */
            return NULL;
        }
    }
    
    if (p_mesh_adv_data->adv_data_length > MESH_PACKET_ADV_OVERHEAD + RBC_MESH_VALUE_MAX_LEN)
    {
        /* invalid length in one of the length fields, discard packet */
        return NULL;
    }

    return p_mesh_adv_data;
}

rbc_mesh_value_handle_t mesh_packet_handle_get(mesh_packet_t* p_packet)
{
    mesh_adv_data_t* p_adv_data = mesh_packet_adv_data_get(p_packet);

    if (p_adv_data == NULL)
    {
        return RBC_MESH_INVALID_HANDLE;
    }
    else
    {
        return p_adv_data->handle;
    }
}

bool mesh_packet_has_additional_data(mesh_packet_t* p_packet)
{
    mesh_adv_data_t* p_mesh_adv_data = (mesh_adv_data_t*) &p_packet->payload[0];
    while (((uint8_t*) p_mesh_adv_data) < &p_packet->payload[0] + (p_packet->header.length - 7))
    {
        if (p_mesh_adv_data->adv_data_type != MESH_ADV_DATA_TYPE || 
            p_mesh_adv_data->mesh_uuid != MESH_UUID)
        {
            return true;
        }
        p_mesh_adv_data += p_mesh_adv_data->adv_data_length + 1;
    }
    
    return false;
}

void mesh_packet_take_ownership(mesh_packet_t* p_packet)
{
    /* some packets may come with additional advertisement fields. These must be
       removed. */
    if (mesh_packet_has_additional_data(p_packet))
    {
        mesh_packet_adv_data_sanitize(p_packet);
    }

    mesh_packet_set_local_addr(p_packet);
}

