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

#ifndef __SPA_LIBPROPS_H__
#define __SPA_LIBPROPS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/props.h>

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
SpaResult       spa_props_get_value   (const SpaProps     *props,
                                       unsigned int        index,
                                       SpaPropValue       *value);

SpaResult       spa_props_copy_values (const SpaProps *src,
                                       SpaProps       *dest);


#ifdef __cplusplus
}  /* extern "C" */
#endif


#endif /* __SPA_LIBPROPS_H__ */
