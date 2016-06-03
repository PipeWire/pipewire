/* Simple Plugin Interface
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPI_PARAMS_H__
#define __SPI_PARAMS_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpiParams SpiParams;

#include <spi/defs.h>

/**
 * SpiParamType:
 */
typedef enum {
  SPI_PARAM_TYPE_INVALID         = 0,
  SPI_PARAM_TYPE_BOOL,
  SPI_PARAM_TYPE_INT8,
  SPI_PARAM_TYPE_UINT8,
  SPI_PARAM_TYPE_INT16,
  SPI_PARAM_TYPE_UINT16,
  SPI_PARAM_TYPE_INT32,
  SPI_PARAM_TYPE_UINT32,
  SPI_PARAM_TYPE_INT64,
  SPI_PARAM_TYPE_UINT64,
  SPI_PARAM_TYPE_FLOAT,
  SPI_PARAM_TYPE_DOUBLE,
  SPI_PARAM_TYPE_STRING,
  SPI_PARAM_TYPE_POINTER,
  SPI_PARAM_TYPE_FRACTION,
  SPI_PARAM_TYPE_BITMASK,
  SPI_PARAM_TYPE_BYTES,
} SpiParamType;

/**
 * SpiParamFlags:
 * @SPI_PARAM_FLAG_NONE: no flags
 * @SPI_PARAM_FLAG_OPTIONAL: the value can be left unset
 * @SPI_PARAM_FLAG_READABLE: param is readable
 * @SPI_PARAM_FLAG_WRITABLE: param is writable
 * @SPI_PARAM_FLAG_READWRITE: param is readable and writable
 * @SPI_PARAM_FLAG_DEPRECATED: param is deprecated and should not be used
 */
typedef enum {
  SPI_PARAM_FLAG_NONE            = 0,
  SPI_PARAM_FLAG_OPTIONAL        = (1 << 0),
  SPI_PARAM_FLAG_READABLE        = (1 << 1),
  SPI_PARAM_FLAG_WRITABLE        = (1 << 2),
  SPI_PARAM_FLAG_READWRITE       = SPI_PARAM_FLAG_READABLE | SPI_PARAM_FLAG_WRITABLE,
  SPI_PARAM_FLAG_DEPRECATED      = (1 << 3),
} SpiParamFlags;

/* SpiParamRangeType:
 * @SPI_PARAM_RANGE_TYPE_NONE: no range specified, full range of type applies
 * @SPI_PARAM_RANGE_TYPE_MIN_MAX: range contains 2 values, min and max
 * @SPI_PARAM_RANGE_TYPE_ENUM: range contains enum of possible values with
 *                            NULL-terminated name
 * @SPI_PARAM_RANGE_TYPE_FLAGS: range contains flags of possible values with
 *                            NULL-terminated name
 */
typedef enum {
  SPI_PARAM_RANGE_TYPE_NONE      = 0,
  SPI_PARAM_RANGE_TYPE_MIN_MAX,
  SPI_PARAM_RANGE_TYPE_ENUM,
  SPI_PARAM_RANGE_TYPE_FLAGS,
} SpiParamRangeType;

/**
 * SpiParamRangeInfo:
 * @name: name of this value
 * @description: user visible description of this value
 * @size: the size of value
 * @value: a possible value
 */
typedef struct {
  const char    *name;
  const char    *description;
  size_t         size;
  const void    *value;
} SpiParamRangeInfo;

/**
 * SpiParamInfo:
 * @id: unique id
 * @name: human readable name
 * @description: description of the param
 * @flags: param flags
 * @type: param type
 * @max_size: maximum size of param value
 * @default_size: size of default value
 * @default_value: default value of param
 * @range_type: type of the range values
 * @range_values: array of possible values
 * @tags: extra tags, NULL terminated
 * @priv: extra private data
 */
typedef struct {
  uint32_t                  id;
  const char               *name;
  const char               *description;
  SpiParamFlags             flags;
  SpiParamType              type;
  size_t                    maxsize;
  size_t                    default_size;
  const void               *default_value;
  SpiParamRangeType         range_type;
  const SpiParamRangeInfo  *range_values;
  const char              **tags;
  const void               *priv;
} SpiParamInfo;

/**
 * SpiParams:
 *
 * Generic parameters.
 */
struct _SpiParams {
  /**
   * SpiParams::get_param_info:
   * @params: a #SpiParams
   * @idx: the param index
   * @info: pointer to a result #SpiParamInfo
   *
   * Gets the information about the parameter at @idx in @params.
   *
   * Returns: #SPI_RESULT_OK on success.
   *          #SPI_RESULT_ENM_END when there is no param info
   *           at @idx. This can be used to iterate the @params.
   */
  SpiResult   (*enum_param_info)   (const SpiParams     *params,
                                    unsigned int         idx,
                                    const SpiParamInfo **infos);
  /**
   * SpiParams::set_param
   * @params: a #SpiParams
   * @id: the param id
   * @type: the value type to set
   * @size: the value size
   * @value: the value to set
   *
   * Sets @value in @param. @type should match the type specified
   * in the #SpiParamInfo for @id or else #SPI_RESULT_WRONG_PARAM_TYPE
   * is returned.
   *
   * Returns: #SPI_RESULT_OK on success.
   *          #SPI_RESULT_INVALID_PARAM_ID when @id is not valid
   *          #SPI_RESULT_WRONG_PARAM_TYPE when @type is not correct
   */
  SpiResult   (*set_param)         (SpiParams           *params,
                                    uint32_t             id,
                                    SpiParamType         type,
                                    size_t               size,
                                    const void          *value);
  /**
   * SpiParams::get_param
   * @params: a #SpiParams
   * @id: the param id
   * @type: a location for the value type
   * @size: a location for the value size
   * @value: a location for the value pointer
   *
   * Get the type, size and value of the parameter with @id.
   *
   * Returns: #SPI_RESULT_OK on success.
   *          #SPI_RESULT_INVALID_PARAM_ID when @id is not valid
   *          #SPI_RESULT_PARAM_UNSET when no value has been set yet
   */
  SpiResult   (*get_param)         (const SpiParams     *params,
                                    uint32_t             id,
                                    SpiParamType        *type,
                                    size_t              *size,
                                    const void         **value);
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPI_PARAMS_H__ */
