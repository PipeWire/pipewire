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

#include <string.h>
#include <spa/defs.h>

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
 */
typedef enum {
  SPA_PROP_FLAG_NONE            = 0,
  SPA_PROP_FLAG_OPTIONAL        = (1 << 0),
  SPA_PROP_FLAG_READABLE        = (1 << 1),
  SPA_PROP_FLAG_WRITABLE        = (1 << 2),
  SPA_PROP_FLAG_READWRITE       = SPA_PROP_FLAG_READABLE | SPA_PROP_FLAG_WRITABLE,
  SPA_PROP_FLAG_DEPRECATED      = (1 << 3),
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
 * SpaPropRangeInfo:
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
} SpaPropRangeInfo;

/**
 * SpaPropInfo:
 * @id: unique id
 * @name: human readable name
 * @description: description of the property
 * @flags: property flags
 * @type: property type
 * @max_size: maximum size of property value
 * @default_size: size of default value
 * @default_value: default value of property
 * @range_type: type of the range values
 * @n_range_values: number of elements in @range_values
 * @range_values: array of possible values
 * @tags: extra tags, NULL terminated
 * @offset: offset in structure with data
 * @mask_offset: offset in structure for the mask
 * @unset_mask: mask to clear when value is set
 * @priv: extra private data
 */
typedef struct {
  uint32_t                  id;
  const char               *name;
  const char               *description;
  SpaPropFlags              flags;
  SpaPropType               type;
  size_t                    maxsize;
  size_t                    default_size;
  const void               *default_value;
  SpaPropRangeType          range_type;
  unsigned int              n_range_values;
  const SpaPropRangeInfo   *range_values;
  const char              **tags;
  size_t                    offset;
  size_t                    mask_offset;
  uint32_t                  unset_mask;
  const void               *priv;
} SpaPropInfo;

typedef struct {
  SpaPropType  type;
  size_t       size;
  const void  *value;
} SpaPropValue;

/**
 * SpaProps:
 *
 * Generic propertiers.
 */
struct _SpaProps {
  /**
   * SpaProps::n_prop_info:
   *
   * The number of items in @prop_info.
   */
  unsigned int n_prop_info;
  /**
   * SpaProps::prop_info:
   *
   * Info about the properties. Can be %NULL when unspecified.
   */
  const SpaPropInfo *prop_info;

  /**
   * SpaProps::set_prop
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
  SpaResult   (*set_prop)         (SpaProps           *props,
                                   unsigned int        index,
                                   const SpaPropValue *value);
  /**
   * SpaProps::get_prop
   * @props: a #SpaProps
   * @index: the property index in the prop_info array
   * @value: a location for the type, size and value
   *
   * Get the type, size and value of the property at @index.
   *
   * Returns: #SPA_RESULT_OK on success.
   *          #SPA_RESULT_INVALID_PROPERTY_INDEX when @index is not valid
   *          #SPA_RESULT_PROPERTY_UNSET when no value has been set yet
   */
  SpaResult   (*get_prop)         (const SpaProps     *props,
                                   unsigned int        index,
                                   SpaPropValue       *value);

  void *priv;
};

#define spa_props_set_prop(p,...)          (p)->set_prop((p),__VA_ARGS__)
#define spa_props_get_prop(p,...)          (p)->get_prop((p),__VA_ARGS__)


static inline unsigned int
spa_props_index_for_id (const SpaProps *props, uint32_t id)
{
  unsigned int i;

  for (i = 0; i < props->n_prop_info; i++) {
     if (props->prop_info[i].id == id)
       return i;
  }
  return -1;
}

static inline unsigned int
spa_props_index_for_name (const SpaProps *props, const char *name)
{
  unsigned int i;

  for (i = 0; i < props->n_prop_info; i++) {
     if (strcmp (props->prop_info[i].name, name) == 0)
       return i;
  }
  return -1;
}


SpaResult       spa_props_generic_set_prop (SpaProps           *props,
                                            unsigned int        index,
                                            const SpaPropValue *value);
SpaResult	spa_props_generic_get_prop (const SpaProps     *props,
                                            unsigned int        index,
                                            SpaPropValue       *value);

SpaResult       spa_props_copy             (const SpaProps *src,
                                            SpaProps       *dest);


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PROPS_H__ */
