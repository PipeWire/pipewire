/* Simple Plugin API
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

#ifndef __SPA_PROPS_H__
#define __SPA_PROPS_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaProps SpaProps;

#define SPA_PROPS_URI             "http://spaplug.in/ns/props"
#define SPA_PROPS_PREFIX          SPA_PROPS_URI "#"

#include <string.h>
#include <spa/defs.h>
#include <spa/dict.h>

/**
 * SpaPropType:
 */
typedef enum {
  SPA_PROP_TYPE_INVALID         = 0,
  SPA_PROP_TYPE_BOOL,
  SPA_PROP_TYPE_INT8,
  SPA_PROP_TYPE_UINT8,
  SPA_PROP_TYPE_INT16,
  SPA_PROP_TYPE_UINT16,
  SPA_PROP_TYPE_INT32,
  SPA_PROP_TYPE_UINT32,
  SPA_PROP_TYPE_INT64,
  SPA_PROP_TYPE_UINT64,
  SPA_PROP_TYPE_INT,
  SPA_PROP_TYPE_UINT,
  SPA_PROP_TYPE_FLOAT,
  SPA_PROP_TYPE_DOUBLE,
  SPA_PROP_TYPE_STRING,
  SPA_PROP_TYPE_RECTANGLE,
  SPA_PROP_TYPE_FRACTION,
  SPA_PROP_TYPE_BITMASK,
  SPA_PROP_TYPE_POINTER
} SpaPropType;

typedef struct {
  uint32_t width;
  uint32_t height;
} SpaRectangle;

typedef struct {
  uint32_t num;
  uint32_t denom;
} SpaFraction;

/**
 * SpaPropFlags:
 * @SPA_PROP_FLAG_NONE: no flags
 * @SPA_PROP_FLAG_OPTIONAL: the value can be left unset
 * @SPA_PROP_FLAG_READABLE: property is readable
 * @SPA_PROP_FLAG_WRITABLE: property is writable
 * @SPA_PROP_FLAG_READWRITE: property is readable and writable
 * @SPA_PROP_FLAG_DEPRECATED: property is deprecated and should not be used
 * @SPA_PROP_FLAG_INFO: property is to get/set the complete structure
 */
typedef enum {
  SPA_PROP_FLAG_NONE            = 0,
  SPA_PROP_FLAG_OPTIONAL        = (1 << 0),
  SPA_PROP_FLAG_READABLE        = (1 << 1),
  SPA_PROP_FLAG_WRITABLE        = (1 << 2),
  SPA_PROP_FLAG_READWRITE       = SPA_PROP_FLAG_READABLE | SPA_PROP_FLAG_WRITABLE,
  SPA_PROP_FLAG_DEPRECATED      = (1 << 3),
  SPA_PROP_FLAG_INFO            = (1 << 4),
} SpaPropFlags;

/* SpaPropRangeType:
 * @SPA_PROP_RANGE_TYPE_NONE: no range specified, full range of type applies
 * @SPA_PROP_RANGE_TYPE_MIN_MAX: range contains 2 values, min and max
 * @SPA_PROP_RANGE_TYPE_STEP: range contains 3 values, min, max and step
 * @SPA_PROP_RANGE_TYPE_ENUM: range contains enum of possible values
 * @SPA_PROP_RANGE_TYPE_FLAGS: range contains flags of possible values
 */
typedef enum {
  SPA_PROP_RANGE_TYPE_NONE      = 0,
  SPA_PROP_RANGE_TYPE_MIN_MAX,
  SPA_PROP_RANGE_TYPE_STEP,
  SPA_PROP_RANGE_TYPE_ENUM,
  SPA_PROP_RANGE_TYPE_FLAGS,
} SpaPropRangeType;

/**
 * SpaPropValue:
 * @size: the property size
 * @value: the property value.
 *
 * The structure to set and get properties.
 */
typedef struct {
  size_t       size;
  const void  *value;
} SpaPropValue;

/**
 * SpaPropRangeInfo:
 * @name: name of this value
 * @val: the value
 */
typedef struct {
  const char    *name;
  SpaPropValue   val;
} SpaPropRangeInfo;

/**
 * SpaPropInfo:
 * @id: unique id
 * @offset: offset in structure with data
 * @name: human readable name
 * @flags: property flags
 * @type: property type
 * @max_size: maximum size of property value
 * @range_type: type of the range values
 * @n_range_values: number of elements in @range_values
 * @range_values: array of possible values
 * @extra: extra info
 */
typedef struct {
  uint32_t                  id;
  size_t                    offset;
  const char               *name;
  SpaPropFlags              flags;
  SpaPropType               type;
  size_t                    maxsize;
  SpaPropRangeType          range_type;
  unsigned int              n_range_values;
  const SpaPropRangeInfo   *range_values;
  SpaDict                  *extra;
} SpaPropInfo;

/**
 * SpaProps:
 * @n_prop_info: number of elements in @prop_info
 * @prop_info: array of #SpaPropInfo. Contains info about the
 *             properties. Can be %NULL when unspecified.
 * @unset_mask: mask of unset properties. For each property in @prop_info there
 *              is a corresponding bit that specifies if the property is currently
 *              unset. When more than 32 properties are present, more uint32_t
 *              fields follow this one.
 *
 * Generic propertiers.
 */
struct _SpaProps {
  unsigned int       n_prop_info;
  const SpaPropInfo *prop_info;
  uint32_t           unset_mask;
};

#define SPA_PROPS_INDEX_IS_UNSET(p,idx) ((&(p)->unset_mask)[(idx) >> 5] & (1U << ((idx) & 31)))
#define SPA_PROPS_INDEX_UNSET(p,idx)    ((&(p)->unset_mask)[(idx) >> 5] |= (1U << ((idx) & 31)))
#define SPA_PROPS_INDEX_SET(p,idx)      ((&(p)->unset_mask)[(idx) >> 5] &= ~(1U << ((idx) & 31)))

static inline unsigned int
spa_props_index_for_id (const SpaProps *props, uint32_t id)
{
  unsigned int i;

  for (i = 0; i < props->n_prop_info; i++) {
     if (props->prop_info[i].id == id)
       return i;
  }
  return SPA_IDX_INVALID;
}

static inline unsigned int
spa_props_index_for_name (const SpaProps *props, const char *name)
{
  unsigned int i;

  for (i = 0; i < props->n_prop_info; i++) {
     if (strcmp (props->prop_info[i].name, name) == 0)
       return i;
  }
  return SPA_IDX_INVALID;
}


/**
 * spa_props_set_value:
 * @props: a #SpaProps
 * @index: the index of the property in the prop_info array
 * @value: the value to set
 *
 * Sets @value in @prop. type should match the type specified
 * in the #SpaPropInfo at @index or else #SPA_RESULT_WRONG_PROPERTY_TYPE
 * is returned.
 *
 * Returns: #SPA_RESULT_OK on success.
 *          #SPA_RESULT_INVALID_PROPERTY_INDEX when @index is not valid
 *          #SPA_RESULT_WRONG_PROPERTY_TYPE when type is not correct
 */
SpaResult       spa_props_set_value   (SpaProps           *props,
                                       unsigned int        index,
                                       const SpaPropValue *value);
/**
 * spa_props_get_value:
 * @props: a #SpaProps
 * @index: the property index in the prop_info array
 * @value: a location for the type, size and value
 *
 * Get the size and value of the property at @index.
 *
 * Returns: #SPA_RESULT_OK on success.
 *          #SPA_RESULT_INVALID_PROPERTY_INDEX when @index is not valid
 *          #SPA_RESULT_PROPERTY_UNSET when no value has been set yet
 */
SpaResult	spa_props_get_value   (const SpaProps     *props,
                                       unsigned int        index,
                                       SpaPropValue       *value);

SpaResult       spa_props_copy_values (const SpaProps *src,
                                       SpaProps       *dest);


size_t          spa_props_get_size    (const SpaProps *props);
size_t          spa_props_serialize   (void *dest, const SpaProps *props);
SpaProps *      spa_props_deserialize (void *src, off_t offset);

SpaProps *      spa_props_copy_into   (void *dest, const SpaProps *props);


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PROPS_H__ */
