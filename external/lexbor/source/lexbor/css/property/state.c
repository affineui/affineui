/*
 * Copyright (C) 2021-2023 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/css/property.h"
#include "lexbor/css/parser.h"
#include "lexbor/css/rule.h"
#include "lexbor/css/value.h"
#include "lexbor/css/unit.h"
#include "lexbor/css/property/state.h"
#include "lexbor/css/property/res.h"
#include "lexbor/core/str.h"

#define LEXBOR_STR_RES_MAP_HEX
#define LEXBOR_STR_RES_MAP_LOWERCASE
#include "lexbor/core/str_res.h"

#include "lexbor/core/conv.h"

#include <string.h>


#define lxb_css_property_state_check_token(parser, token)                     \
    if ((token) == NULL) {                                                    \
        return lxb_css_parser_memory_fail(parser);                            \
    }

#define lxb_css_property_state_get_type(parser, token, type)                  \
    do {                                                                      \
        lxb_css_syntax_parser_consume(parser);                                \
                                                                              \
        token = lxb_css_syntax_parser_token_wo_ws(parser);                    \
        lxb_css_property_state_check_token(parser, token);                    \
                                                                              \
        if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {                      \
            return lxb_css_parser_success(parser);                            \
        }                                                                     \
                                                                              \
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data, \
                                  lxb_css_syntax_token_ident(token)->length); \
    }                                                                         \
    while (false)

#define LXB_CSS_PROPERTY_STATE_HEX_MASK(n)                                    \
    ((((uint32_t) 1 << (32 - (n))) - 1) << (n))


static bool
lxb_css_property_state_color_rgba_old(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token,
                                      lxb_css_value_color_t *color);
static bool
lxb_css_property_state_color_hsla_old(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token,
                                      lxb_css_value_color_hsla_t *hsl);
static bool
lxb_css_property_state_is_global(lxb_css_value_type_t type);

static bool
lxb_css_property_state_length(lxb_css_parser_t *parser,
                              const lxb_css_syntax_token_t *token,
                              lxb_css_value_length_t *length)
{
    const lxb_css_data_t *unit;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
            unit = lxb_css_unit_absolute_relative_by_name(lxb_css_syntax_token_dimension(token)->str.data,
                                                          lxb_css_syntax_token_dimension(token)->str.length);
            if (unit == NULL) {
                return false;
            }

            length->num = lxb_css_syntax_token_dimension(token)->num.num;
            length->is_float = lxb_css_syntax_token_dimension(token)->num.is_float;
            length->unit = (lxb_css_unit_t) unit->unique;
            break;

        case LXB_CSS_SYNTAX_TOKEN_NUMBER:
            if (lxb_css_syntax_token_number(token)->num != 0) {
                return false;
            }

            length->num = lxb_css_syntax_token_number(token)->num;
            length->is_float = lxb_css_syntax_token_number(token)->is_float;
            length->unit = LXB_CSS_UNIT__UNDEF;
            break;

        default:
            return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_length_percentage(lxb_css_parser_t *parser,
                                         const lxb_css_syntax_token_t *token,
                                         lxb_css_value_length_percentage_t *lp)
{
    const lxb_css_data_t *unit;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
            unit = lxb_css_unit_absolute_relative_by_name(lxb_css_syntax_token_dimension(token)->str.data,
                                                          lxb_css_syntax_token_dimension(token)->str.length);
            if (unit == NULL) {
                return false;
            }

            lp->type = LXB_CSS_VALUE__LENGTH;
            lp->u.length.num = lxb_css_syntax_token_dimension(token)->num.num;
            lp->u.length.is_float = lxb_css_syntax_token_dimension(token)->num.is_float;
            lp->u.length.unit = (lxb_css_unit_t) unit->unique;
            break;

        case LXB_CSS_SYNTAX_TOKEN_NUMBER:
            if (lxb_css_syntax_token_number(token)->num != 0) {
                return false;
            }

            lp->type = LXB_CSS_VALUE__NUMBER;
            lp->u.length.num = lxb_css_syntax_token_number(token)->num;
            lp->u.length.is_float = lxb_css_syntax_token_number(token)->is_float;
            lp->u.length.unit = LXB_CSS_UNIT__UNDEF;
            break;

        case LXB_CSS_SYNTAX_TOKEN_PERCENTAGE:
            lp->type = LXB_CSS_VALUE__PERCENTAGE;
            lp->u.percentage.num = lxb_css_syntax_token_percentage(token)->num;
            lp->u.percentage.is_float = lxb_css_syntax_token_percentage(token)->is_float;
            break;

        default:
            return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_number_length_percentage(lxb_css_parser_t *parser,
                                                const lxb_css_syntax_token_t *token,
                                                lxb_css_value_number_length_percentage_t *nlp)
{
    const lxb_css_data_t *unit;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
            unit = lxb_css_unit_absolute_relative_by_name(lxb_css_syntax_token_dimension(token)->str.data,
                                                          lxb_css_syntax_token_dimension(token)->str.length);
            if (unit == NULL) {
                return false;
            }

            nlp->type = LXB_CSS_VALUE__LENGTH;
            nlp->u.length.num = lxb_css_syntax_token_dimension(token)->num.num;
            nlp->u.length.is_float = lxb_css_syntax_token_dimension(token)->num.is_float;
            nlp->u.length.unit = (lxb_css_unit_t) unit->unique;
            break;

        case LXB_CSS_SYNTAX_TOKEN_NUMBER:
            nlp->type = LXB_CSS_VALUE__NUMBER;
            nlp->u.number.num = lxb_css_syntax_token_number(token)->num;
            nlp->u.number.is_float = lxb_css_syntax_token_number(token)->is_float;
            break;

        case LXB_CSS_SYNTAX_TOKEN_PERCENTAGE:
            nlp->type = LXB_CSS_VALUE__PERCENTAGE;
            nlp->u.percentage.num = lxb_css_syntax_token_percentage(token)->num;
            nlp->u.percentage.is_float = lxb_css_syntax_token_percentage(token)->is_float;
            break;

        default:
            return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_number_length(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token,
                                     lxb_css_value_number_length_t *nl)
{
    const lxb_css_data_t *unit;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
            unit = lxb_css_unit_absolute_relative_by_name(lxb_css_syntax_token_dimension(token)->str.data,
                                                          lxb_css_syntax_token_dimension(token)->str.length);
            if (unit == NULL) {
                return false;
            }

            nl->type = LXB_CSS_VALUE__LENGTH;
            nl->u.length.num = lxb_css_syntax_token_dimension(token)->num.num;
            nl->u.length.is_float = lxb_css_syntax_token_dimension(token)->num.is_float;
            nl->u.length.unit = (lxb_css_unit_t) unit->unique;
            break;

        case LXB_CSS_SYNTAX_TOKEN_NUMBER:
            nl->type = LXB_CSS_VALUE__NUMBER;
            nl->u.number.num = lxb_css_syntax_token_number(token)->num;
            nl->u.number.is_float = lxb_css_syntax_token_number(token)->is_float;
            break;

        default:
            return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_number(lxb_css_parser_t *parser,
                              const lxb_css_syntax_token_t *token,
                              lxb_css_value_number_t *number)
{
    if (token->type != LXB_CSS_SYNTAX_TOKEN_NUMBER) {
        return false;
    }

    number->num = lxb_css_syntax_token_number(token)->num;
    number->is_float = lxb_css_syntax_token_number(token)->is_float;

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_integer(lxb_css_parser_t *parser,
                               const lxb_css_syntax_token_t *token,
                               lxb_css_value_integer_t *intg)
{
    long ln;
    double num;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_NUMBER) {
        return false;
    }

    num = lxb_css_syntax_token_number(token)->num;
    ln = lexbor_conv_double_to_long(num);

    num = num - (double) ln;

    if (num < 0.0 || num > 0.0) {
        return false;
    }

    intg->num = ln;

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_percentage(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token,
                                  lxb_css_value_percentage_t *perc)
{
    if (token->type != LXB_CSS_SYNTAX_TOKEN_PERCENTAGE) {
        return false;
    }

    perc->num = lxb_css_syntax_token_percentage(token)->num;
    perc->is_float = lxb_css_syntax_token_percentage(token)->is_float;

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_number_percentage_none(lxb_css_parser_t *parser,
                                              const lxb_css_syntax_token_t *token,
                                              lxb_css_value_number_percentage_t *np)
{
    double num;
    lxb_css_value_type_t type;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_NUMBER) {
        np->type = LXB_CSS_VALUE__NUMBER;
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE) {
        np->type = LXB_CSS_VALUE__PERCENTAGE;
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        if (type != LXB_CSS_VALUE_NONE) {
            return false;
        }

        np->type = LXB_CSS_VALUE_NONE;

        lxb_css_syntax_parser_consume(parser);

        return true;
    }
    else {
        return false;
    }

    num = lxb_css_syntax_token_number(token)->num;

    np->u.number.num = num;
    np->u.number.is_float = lxb_css_syntax_token_number(token)->is_float;

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_percentage_none(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token,
                                       lxb_css_value_percentage_type_t *np)
{
    double num;
    lxb_css_value_type_t type;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE) {
        np->type = LXB_CSS_VALUE__PERCENTAGE;
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        if (type != LXB_CSS_VALUE_NONE) {
            return false;
        }

        np->type = LXB_CSS_VALUE_NONE;

        lxb_css_syntax_parser_consume(parser);

        return true;
    }
    else {
        return false;
    }

    num = lxb_css_syntax_token_number(token)->num;

    np->percentage.num = num;
    np->percentage.is_float = lxb_css_syntax_token_number(token)->is_float;

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_number_percentage(lxb_css_parser_t *parser,
                                         const lxb_css_syntax_token_t *token,
                                         lxb_css_value_number_percentage_t *np)
{
    double num;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_NUMBER) {
        np->type = LXB_CSS_VALUE__NUMBER;
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE) {
        np->type = LXB_CSS_VALUE__PERCENTAGE;
    }
    else {
        return false;
    }

    num = lxb_css_syntax_token_number(token)->num;

    np->u.number.num = num;
    np->u.number.is_float = lxb_css_syntax_token_number(token)->is_float;

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_angle(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token,
                             lxb_css_value_angle_t *angle)
{
    const lxb_css_data_t *unit;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_DIMENSION) {
        return false;
    }

    unit = lxb_css_unit_angel_by_name(lxb_css_syntax_token_dimension(token)->str.data,
                                      lxb_css_syntax_token_dimension(token)->str.length);
    if (unit == NULL) {
        return false;
    }

    angle->num = lxb_css_syntax_token_dimension(token)->num.num;
    angle->is_float = lxb_css_syntax_token_dimension(token)->num.is_float;
    angle->unit = (lxb_css_unit_angel_t) unit->unique;

    lxb_css_syntax_parser_consume(parser);

    return true;
}

bool
lxb_css_property_state_width_handler(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token,
                                     lxb_css_property_width_t *width)
{
    lxb_css_value_type_t type;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_VALUE_AUTO:
            case LXB_CSS_VALUE_MIN_CONTENT:
            case LXB_CSS_VALUE_MAX_CONTENT:
                width->type = type;
                break;

            default:
                return false;
        }

        lxb_css_syntax_parser_consume(parser);

        return true;
    }

    return lxb_css_property_state_length_percentage(parser, token,
                                   (lxb_css_value_length_percentage_t *)width);
}

static bool
lxb_css_property_state_hue(lxb_css_parser_t *parser,
                           const lxb_css_syntax_token_t *token,
                           lxb_css_value_hue_t *hue)
{
    const lxb_css_data_t *unit;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
            unit = lxb_css_unit_angel_by_name(lxb_css_syntax_token_dimension(token)->str.data,
                                              lxb_css_syntax_token_dimension(token)->str.length);
            if (unit == NULL) {
                return false;
            }

            hue->type = LXB_CSS_VALUE__ANGLE;
            hue->u.angle.num = lxb_css_syntax_token_dimension(token)->num.num;
            hue->u.angle.is_float = lxb_css_syntax_token_dimension(token)->num.is_float;
            hue->u.angle.unit = (lxb_css_unit_angel_t) unit->unique;
            break;

        case LXB_CSS_SYNTAX_TOKEN_NUMBER:
            hue->type = LXB_CSS_VALUE__NUMBER;
            hue->u.number.num = lxb_css_syntax_token_number(token)->num;
            hue->u.number.is_float = lxb_css_syntax_token_number(token)->is_float;
            break;

        default:
            return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

lxb_inline bool
lxb_css_property_state_hue_none(lxb_css_parser_t *parser,
                                const lxb_css_syntax_token_t *token,
                                lxb_css_value_hue_t *hue)
{
    lxb_css_value_type_t type;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_property_state_hue(parser, token, hue);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    if (type != LXB_CSS_VALUE_NONE) {
        return false;
    }

    hue->type = LXB_CSS_VALUE_NONE;

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_color_hex(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token,
                                 lxb_css_value_color_t *color)
{
    size_t length;
    uint32_t chex;
    lxb_char_t ch;
    const lxb_char_t *end, *p;
    lxb_css_value_color_hex_rgba_t *rgba;

    length = token->types.hash.length;

    if (length > 8) {
        return false;
    }

    p = token->types.hash.data;
    end = p + length;

    chex = 0;

    while (p < end) {
        ch = lexbor_str_res_map_lowercase[lexbor_str_res_map_hex[*p]];

        if (ch == 0xff) {
            return false;
        }

        chex = chex << 4 | ch;

        p++;
    }

    rgba = &color->u.hex.rgba;

    switch (length) {
        case 3:
            rgba->r = chex >> 8;
            rgba->g = chex >> 4 & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(4);
            rgba->b = chex & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(4);
            rgba->a = 0xff;

            color->u.hex.type = LXB_CSS_PROPERTY_COLOR_HEX_TYPE_3;
            break;

        case 4:
            rgba->r = chex >> 12;
            rgba->g = chex >> 8 & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(4);
            rgba->b = chex >> 4 & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(4);
            rgba->a = chex & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(4);

            color->u.hex.type = LXB_CSS_PROPERTY_COLOR_HEX_TYPE_4;
            break;

        case 6:
            rgba->r = chex >> 16;
            rgba->g = chex >> 8 & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(8);
            rgba->b = chex & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(8);
            rgba->a = 0xff;

            color->u.hex.type = LXB_CSS_PROPERTY_COLOR_HEX_TYPE_6;
            break;

        case 8:
            rgba->r = chex >> 24;
            rgba->g = chex >> 16 & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(8);
            rgba->b = chex >> 8 & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(8);
            rgba->a = chex & ~LXB_CSS_PROPERTY_STATE_HEX_MASK(8);

            color->u.hex.type = LXB_CSS_PROPERTY_COLOR_HEX_TYPE_8;
            break;

        default:
            return false;
    }

    color->type = LXB_CSS_COLOR_HEX;

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_color_rgba(lxb_css_parser_t *parser,
                                  lxb_css_value_color_t *color)
{
    bool res;
    lxb_css_color_type_t type;
    lxb_css_value_color_rgba_t *rgb;
    const lxb_css_syntax_token_t *token;

    rgb = &color->u.rgb;

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage_none(parser, token, &rgb->r);
    if (res == false) {
        return false;
    }

    type = rgb->r.type;

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_COMMA) {
        /* Deprecated format. */

        if (type == LXB_CSS_VALUE_NONE) {
            return false;
        }

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        rgb->old = true;

        return lxb_css_property_state_color_rgba_old(parser, token, color);
    }

    res = lxb_css_property_state_number_percentage_none(parser, token, &rgb->g);
    if (res == false) {
        return false;
    }

    if (type != rgb->g.type) {
        if (type == LXB_CSS_VALUE_NONE) {
            type = rgb->g.type;
        }
        else if (rgb->g.type != LXB_CSS_VALUE_NONE) {
            return false;
        }
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage_none(parser, token, &rgb->b);
    if (res == false) {
        return false;
    }

    if (type != rgb->b.type && type != LXB_CSS_VALUE_NONE
        && rgb->b.type != LXB_CSS_VALUE_NONE)
    {
            return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_DELIM) {
        if (lxb_css_syntax_token_delim(token)->character != '/') {
            return false;
        }

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        lxb_css_syntax_parser_consume(parser);
        return true;
    }
    else {
        return false;
    }

    res = lxb_css_property_state_number_percentage_none(parser, token, &rgb->a);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_color_rgba_old(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token,
                                      lxb_css_value_color_t *color)
{
    bool res;
    lxb_css_value_color_rgba_t *rgb;

    rgb = &color->u.rgb;

    res = lxb_css_property_state_number_percentage(parser, token, &rgb->g);
    if (res == false) {
        return false;
    }

    if (rgb->r.type != rgb->g.type) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage(parser, token, &rgb->b);
    if (res == false) {
        return false;
    }

    if (rgb->r.type != rgb->b.type) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        lxb_css_syntax_parser_consume(parser);
        return true;
    }
    else if (token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage(parser, token, &rgb->a);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_color_hsla(lxb_css_parser_t *parser,
                                  lxb_css_value_color_t *color)
{
    bool res;
    lxb_css_value_color_hsla_t *hsl;
    const lxb_css_syntax_token_t *token;

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    hsl = &color->u.hsl;

    res = lxb_css_property_state_hue_none(parser, token, &hsl->h);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_COMMA) {
        /* Deprecated format. */

        if (hsl->h.type == LXB_CSS_VALUE_NONE) {
            return false;
        }

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        hsl->old = true;

        return lxb_css_property_state_color_hsla_old(parser, token, hsl);
    }

    res = lxb_css_property_state_percentage_none(parser, token, &hsl->s);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_percentage_none(parser, token, &hsl->l);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_DELIM) {
        if (lxb_css_syntax_token_delim(token)->character != '/') {
            return false;
        }

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        lxb_css_syntax_parser_consume(parser);
        return true;
    }

    res = lxb_css_property_state_number_percentage_none(parser, token, &hsl->a);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_color_hsla_old(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token,
                                      lxb_css_value_color_hsla_t *hsl)
{
    bool res;

    res = lxb_css_property_state_percentage(parser, token, &hsl->s.percentage);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_percentage(parser, token, &hsl->l.percentage);
    if (res == false) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        goto done;
    }
    else if (token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage(parser, token, &hsl->a);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        return false;
    }

done:

    lxb_css_syntax_parser_consume(parser);

    hsl->s.type = LXB_CSS_VALUE__PERCENTAGE;
    hsl->l.type = LXB_CSS_VALUE__PERCENTAGE;

    return true;
}

static bool
lxb_css_property_state_color_lab(lxb_css_parser_t *parser,
                                 lxb_css_value_color_t *color)
{
    bool res;
    lxb_css_value_color_lab_t *lab;
    const lxb_css_syntax_token_t *token;

    lab = &color->u.lab;

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage_none(parser, token, &lab->l);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage_none(parser, token, &lab->a);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage_none(parser, token, &lab->b);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_DELIM) {
        if (lxb_css_syntax_token_delim(token)->character != '/') {
            return false;
        }

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        lxb_css_syntax_parser_consume(parser);
        return true;
    }
    else {
        return false;
    }

    res = lxb_css_property_state_number_percentage_none(parser, token,
                                                        &lab->alpha);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

static bool
lxb_css_property_state_color_lch(lxb_css_parser_t *parser,
                                 lxb_css_value_color_t *color)
{
    bool res;
    lxb_css_value_color_lch_t *lch;
    const lxb_css_syntax_token_t *token;

    lch = &color->u.lch;

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage_none(parser, token, &lch->l);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number_percentage_none(parser, token, &lch->c);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_hue_none(parser, token, &lch->h);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_DELIM) {
        if (lxb_css_syntax_token_delim(token)->character != '/') {
            return false;
        }

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        lxb_css_syntax_parser_consume(parser);
        return true;
    }
    else {
        return false;
    }

    res = lxb_css_property_state_number_percentage_none(parser, token, &lch->a);
    if (res == false) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

/*
 * Return:
 *     true  and status always LXB_STATUS_OK — token consumed, ok.
 *     false and status != LXB_STATUS_OK     — token consumed, not ok.
 *     false and status == LXB_STATUS_OK     — token not consumed, not ok.
 */
static bool
lxb_css_property_state_color_handler(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token,
                                     lxb_css_value_color_t *color,
                                     lxb_status_t *status)
{
    bool res;
    lxb_css_value_type_t type;

    *status = LXB_STATUS_OK;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_HASH:
            color->type = LXB_CSS_VALUE_HEX;

            return lxb_css_property_state_color_hex(parser, token, color);

        case LXB_CSS_SYNTAX_TOKEN_FUNCTION:
            type = lxb_css_value_by_name(lxb_css_syntax_token_function(token)->data,
                                         lxb_css_syntax_token_function(token)->length);
            color->type = type;

            switch (type) {
                /* <color> */
                case LXB_CSS_VALUE_RGB:
                case LXB_CSS_VALUE_RGBA:
                    res = lxb_css_property_state_color_rgba(parser, color);
                    break;

                case LXB_CSS_VALUE_HSL:
                case LXB_CSS_VALUE_HSLA:
                case LXB_CSS_VALUE_HWB:
                    res = lxb_css_property_state_color_hsla(parser, color);
                    break;

                case LXB_CSS_VALUE_LAB:
                case LXB_CSS_VALUE_OKLAB:
                    res = lxb_css_property_state_color_lab(parser, color);
                    break;

                case LXB_CSS_VALUE_LCH:
                case LXB_CSS_VALUE_OKLCH:
                    res = lxb_css_property_state_color_lch(parser, color);
                    break;

                case LXB_CSS_VALUE_COLOR:
                default:
                    *status = LXB_STATUS_OK;
                    return false;
            }

            if (!res) {
                *status = LXB_STATUS_ERROR_UNEXPECTED_DATA;
            }

            return res;

        case LXB_CSS_SYNTAX_TOKEN_IDENT:
            type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                         lxb_css_syntax_token_ident(token)->length);
            switch (type) {
                /* <color> */
                case LXB_CSS_VALUE_CURRENTCOLOR:
                /* <system-color> */
                case LXB_CSS_VALUE_CANVAS:
                case LXB_CSS_VALUE_CANVASTEXT:
                case LXB_CSS_VALUE_LINKTEXT:
                case LXB_CSS_VALUE_VISITEDTEXT:
                case LXB_CSS_VALUE_ACTIVETEXT:
                case LXB_CSS_VALUE_BUTTONFACE:
                case LXB_CSS_VALUE_BUTTONTEXT:
                case LXB_CSS_VALUE_BUTTONBORDER:
                case LXB_CSS_VALUE_FIELD:
                case LXB_CSS_VALUE_FIELDTEXT:
                case LXB_CSS_VALUE_HIGHLIGHT:
                case LXB_CSS_VALUE_HIGHLIGHTTEXT:
                case LXB_CSS_VALUE_SELECTEDITEM:
                case LXB_CSS_VALUE_SELECTEDITEMTEXT:
                case LXB_CSS_VALUE_MARK:
                case LXB_CSS_VALUE_MARKTEXT:
                case LXB_CSS_VALUE_GRAYTEXT:
                case LXB_CSS_VALUE_ACCENTCOLOR:
                case LXB_CSS_VALUE_ACCENTCOLORTEXT:
                /* <absolute-color-base> */
                case LXB_CSS_VALUE_TRANSPARENT:
                /* <named-color> */
                case LXB_CSS_VALUE_ALICEBLUE:
                case LXB_CSS_VALUE_ANTIQUEWHITE:
                case LXB_CSS_VALUE_AQUA:
                case LXB_CSS_VALUE_AQUAMARINE:
                case LXB_CSS_VALUE_AZURE:
                case LXB_CSS_VALUE_BEIGE:
                case LXB_CSS_VALUE_BISQUE:
                case LXB_CSS_VALUE_BLACK:
                case LXB_CSS_VALUE_BLANCHEDALMOND:
                case LXB_CSS_VALUE_BLUE:
                case LXB_CSS_VALUE_BLUEVIOLET:
                case LXB_CSS_VALUE_BROWN:
                case LXB_CSS_VALUE_BURLYWOOD:
                case LXB_CSS_VALUE_CADETBLUE:
                case LXB_CSS_VALUE_CHARTREUSE:
                case LXB_CSS_VALUE_CHOCOLATE:
                case LXB_CSS_VALUE_CORAL:
                case LXB_CSS_VALUE_CORNFLOWERBLUE:
                case LXB_CSS_VALUE_CORNSILK:
                case LXB_CSS_VALUE_CRIMSON:
                case LXB_CSS_VALUE_CYAN:
                case LXB_CSS_VALUE_DARKBLUE:
                case LXB_CSS_VALUE_DARKCYAN:
                case LXB_CSS_VALUE_DARKGOLDENROD:
                case LXB_CSS_VALUE_DARKGRAY:
                case LXB_CSS_VALUE_DARKGREEN:
                case LXB_CSS_VALUE_DARKGREY:
                case LXB_CSS_VALUE_DARKKHAKI:
                case LXB_CSS_VALUE_DARKMAGENTA:
                case LXB_CSS_VALUE_DARKOLIVEGREEN:
                case LXB_CSS_VALUE_DARKORANGE:
                case LXB_CSS_VALUE_DARKORCHID:
                case LXB_CSS_VALUE_DARKRED:
                case LXB_CSS_VALUE_DARKSALMON:
                case LXB_CSS_VALUE_DARKSEAGREEN:
                case LXB_CSS_VALUE_DARKSLATEBLUE:
                case LXB_CSS_VALUE_DARKSLATEGRAY:
                case LXB_CSS_VALUE_DARKSLATEGREY:
                case LXB_CSS_VALUE_DARKTURQUOISE:
                case LXB_CSS_VALUE_DARKVIOLET:
                case LXB_CSS_VALUE_DEEPPINK:
                case LXB_CSS_VALUE_DEEPSKYBLUE:
                case LXB_CSS_VALUE_DIMGRAY:
                case LXB_CSS_VALUE_DIMGREY:
                case LXB_CSS_VALUE_DODGERBLUE:
                case LXB_CSS_VALUE_FIREBRICK:
                case LXB_CSS_VALUE_FLORALWHITE:
                case LXB_CSS_VALUE_FORESTGREEN:
                case LXB_CSS_VALUE_FUCHSIA:
                case LXB_CSS_VALUE_GAINSBORO:
                case LXB_CSS_VALUE_GHOSTWHITE:
                case LXB_CSS_VALUE_GOLD:
                case LXB_CSS_VALUE_GOLDENROD:
                case LXB_CSS_VALUE_GRAY:
                case LXB_CSS_VALUE_GREEN:
                case LXB_CSS_VALUE_GREENYELLOW:
                case LXB_CSS_VALUE_GREY:
                case LXB_CSS_VALUE_HONEYDEW:
                case LXB_CSS_VALUE_HOTPINK:
                case LXB_CSS_VALUE_INDIANRED:
                case LXB_CSS_VALUE_INDIGO:
                case LXB_CSS_VALUE_IVORY:
                case LXB_CSS_VALUE_KHAKI:
                case LXB_CSS_VALUE_LAVENDER:
                case LXB_CSS_VALUE_LAVENDERBLUSH:
                case LXB_CSS_VALUE_LAWNGREEN:
                case LXB_CSS_VALUE_LEMONCHIFFON:
                case LXB_CSS_VALUE_LIGHTBLUE:
                case LXB_CSS_VALUE_LIGHTCORAL:
                case LXB_CSS_VALUE_LIGHTCYAN:
                case LXB_CSS_VALUE_LIGHTGOLDENRODYELLOW:
                case LXB_CSS_VALUE_LIGHTGRAY:
                case LXB_CSS_VALUE_LIGHTGREEN:
                case LXB_CSS_VALUE_LIGHTGREY:
                case LXB_CSS_VALUE_LIGHTPINK:
                case LXB_CSS_VALUE_LIGHTSALMON:
                case LXB_CSS_VALUE_LIGHTSEAGREEN:
                case LXB_CSS_VALUE_LIGHTSKYBLUE:
                case LXB_CSS_VALUE_LIGHTSLATEGRAY:
                case LXB_CSS_VALUE_LIGHTSLATEGREY:
                case LXB_CSS_VALUE_LIGHTSTEELBLUE:
                case LXB_CSS_VALUE_LIGHTYELLOW:
                case LXB_CSS_VALUE_LIME:
                case LXB_CSS_VALUE_LIMEGREEN:
                case LXB_CSS_VALUE_LINEN:
                case LXB_CSS_VALUE_MAGENTA:
                case LXB_CSS_VALUE_MAROON:
                case LXB_CSS_VALUE_MEDIUMAQUAMARINE:
                case LXB_CSS_VALUE_MEDIUMBLUE:
                case LXB_CSS_VALUE_MEDIUMORCHID:
                case LXB_CSS_VALUE_MEDIUMPURPLE:
                case LXB_CSS_VALUE_MEDIUMSEAGREEN:
                case LXB_CSS_VALUE_MEDIUMSLATEBLUE:
                case LXB_CSS_VALUE_MEDIUMSPRINGGREEN:
                case LXB_CSS_VALUE_MEDIUMTURQUOISE:
                case LXB_CSS_VALUE_MEDIUMVIOLETRED:
                case LXB_CSS_VALUE_MIDNIGHTBLUE:
                case LXB_CSS_VALUE_MINTCREAM:
                case LXB_CSS_VALUE_MISTYROSE:
                case LXB_CSS_VALUE_MOCCASIN:
                case LXB_CSS_VALUE_NAVAJOWHITE:
                case LXB_CSS_VALUE_NAVY:
                case LXB_CSS_VALUE_OLDLACE:
                case LXB_CSS_VALUE_OLIVE:
                case LXB_CSS_VALUE_OLIVEDRAB:
                case LXB_CSS_VALUE_ORANGE:
                case LXB_CSS_VALUE_ORANGERED:
                case LXB_CSS_VALUE_ORCHID:
                case LXB_CSS_VALUE_PALEGOLDENROD:
                case LXB_CSS_VALUE_PALEGREEN:
                case LXB_CSS_VALUE_PALETURQUOISE:
                case LXB_CSS_VALUE_PALEVIOLETRED:
                case LXB_CSS_VALUE_PAPAYAWHIP:
                case LXB_CSS_VALUE_PEACHPUFF:
                case LXB_CSS_VALUE_PERU:
                case LXB_CSS_VALUE_PINK:
                case LXB_CSS_VALUE_PLUM:
                case LXB_CSS_VALUE_POWDERBLUE:
                case LXB_CSS_VALUE_PURPLE:
                case LXB_CSS_VALUE_REBECCAPURPLE:
                case LXB_CSS_VALUE_RED:
                case LXB_CSS_VALUE_ROSYBROWN:
                case LXB_CSS_VALUE_ROYALBLUE:
                case LXB_CSS_VALUE_SADDLEBROWN:
                case LXB_CSS_VALUE_SALMON:
                case LXB_CSS_VALUE_SANDYBROWN:
                case LXB_CSS_VALUE_SEAGREEN:
                case LXB_CSS_VALUE_SEASHELL:
                case LXB_CSS_VALUE_SIENNA:
                case LXB_CSS_VALUE_SILVER:
                case LXB_CSS_VALUE_SKYBLUE:
                case LXB_CSS_VALUE_SLATEBLUE:
                case LXB_CSS_VALUE_SLATEGRAY:
                case LXB_CSS_VALUE_SLATEGREY:
                case LXB_CSS_VALUE_SNOW:
                case LXB_CSS_VALUE_SPRINGGREEN:
                case LXB_CSS_VALUE_STEELBLUE:
                case LXB_CSS_VALUE_TAN:
                case LXB_CSS_VALUE_TEAL:
                case LXB_CSS_VALUE_THISTLE:
                case LXB_CSS_VALUE_TOMATO:
                case LXB_CSS_VALUE_TURQUOISE:
                case LXB_CSS_VALUE_VIOLET:
                case LXB_CSS_VALUE_WHEAT:
                case LXB_CSS_VALUE_WHITE:
                case LXB_CSS_VALUE_WHITESMOKE:
                case LXB_CSS_VALUE_YELLOW:
                case LXB_CSS_VALUE_YELLOWGREEN:
                    color->type = type;
                    break;

                default:
                    return false;
            }

            break;

        default:
            return false;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;
}

bool
lxb_css_property_state__undef(lxb_css_parser_t *parser,
                              const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state__custom(lxb_css_parser_t *parser,
                               const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_status_t status;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property__custom_t *custom = declar->u.custom;

    (void) lexbor_str_init(&custom->value, parser->memory->mraw, 0);
    if (custom->value.data == NULL) {
        return lxb_css_parser_memory_fail(parser);
    }

    while (token != NULL && token->type != LXB_CSS_SYNTAX_TOKEN__END) {
        status = lxb_css_syntax_token_serialize_str(token, &custom->value,
                                                    parser->memory->mraw);
        if (status != LXB_STATUS_OK) {
            return lxb_css_parser_memory_fail(parser);
        }

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token(parser);
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_display(lxb_css_parser_t *parser,
                               const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_property_display_t *display;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    display = declar->u.display;

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);

    switch (type) {
        /* <display-outside> */
        case LXB_CSS_DISPLAY_BLOCK:
        case LXB_CSS_DISPLAY_INLINE:
        case LXB_CSS_DISPLAY_RUN_IN:
            display->a = type;
            goto inside_listitem;

        /* <display-inside> */
        case LXB_CSS_DISPLAY_FLOW:
        case LXB_CSS_DISPLAY_FLOW_ROOT:
            display->a = type;
            goto outside_listitem;

        case LXB_CSS_DISPLAY_TABLE:
        case LXB_CSS_DISPLAY_FLEX:
        case LXB_CSS_DISPLAY_GRID:
        case LXB_CSS_DISPLAY_RUBY:
            display->a = type;
            goto outside;

        /* <display-internal> */
        case LXB_CSS_DISPLAY_LIST_ITEM:
            display->a = type;
            goto listitem_only;

        /* <display-internal> */
        case LXB_CSS_DISPLAY_TABLE_ROW_GROUP:
        case LXB_CSS_DISPLAY_TABLE_HEADER_GROUP:
        case LXB_CSS_DISPLAY_TABLE_FOOTER_GROUP:
        case LXB_CSS_DISPLAY_TABLE_ROW:
        case LXB_CSS_DISPLAY_TABLE_CELL:
        case LXB_CSS_DISPLAY_TABLE_COLUMN_GROUP:
        case LXB_CSS_DISPLAY_TABLE_COLUMN:
        case LXB_CSS_DISPLAY_TABLE_CAPTION:
        case LXB_CSS_DISPLAY_RUBY_BASE:
        case LXB_CSS_DISPLAY_RUBY_TEXT:
        case LXB_CSS_DISPLAY_RUBY_BASE_CONTAINER:
        case LXB_CSS_DISPLAY_RUBY_TEXT_CONTAINER:
        /* <display-box> */
        case LXB_CSS_DISPLAY_CONTENTS:
        case LXB_CSS_DISPLAY_NONE:
        /* <display-legacy> */
        case LXB_CSS_DISPLAY_INLINE_BLOCK:
        case LXB_CSS_DISPLAY_INLINE_TABLE:
        case LXB_CSS_DISPLAY_INLINE_FLEX:
        case LXB_CSS_DISPLAY_INLINE_GRID:
            display->a = type;
            goto done;

        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            display->a = type;
            goto done;

        default:
            return lxb_css_parser_failed(parser);
    }

inside_listitem:

    lxb_css_property_state_get_type(parser, token, type);

    switch (type) {
        /* <display-inside> */
        case LXB_CSS_DISPLAY_FLOW:
        case LXB_CSS_DISPLAY_FLOW_ROOT:
            display->b = type;
            break;

        case LXB_CSS_DISPLAY_TABLE:
        case LXB_CSS_DISPLAY_FLEX:
        case LXB_CSS_DISPLAY_GRID:
        case LXB_CSS_DISPLAY_RUBY:
            display->b = type;
            goto done;

        case LXB_CSS_DISPLAY_LIST_ITEM:
            display->b = type;
            goto flow_only;

        default:
            return lxb_css_parser_failed(parser);
    }

listitem:

    lxb_css_property_state_get_type(parser, token, type);

    if (type == LXB_CSS_DISPLAY_LIST_ITEM) {
        display->c = type;
        goto done;
    }

    return lxb_css_parser_failed(parser);

outside:

    lxb_css_property_state_get_type(parser, token, type);

    switch (type) {
        /* <display-outside> */
        case LXB_CSS_DISPLAY_BLOCK:
        case LXB_CSS_DISPLAY_INLINE:
        case LXB_CSS_DISPLAY_RUN_IN:
            if (display->b == LXB_CSS_PROPERTY__UNDEF) {
                display->b = type;
            }
            else {
                display->c = type;
            }

            goto done;

        default:
            return lxb_css_parser_failed(parser);
    }

outside_listitem:

    lxb_css_property_state_get_type(parser, token, type);

    switch (type) {
        /* <display-outside> */
        case LXB_CSS_DISPLAY_BLOCK:
        case LXB_CSS_DISPLAY_INLINE:
        case LXB_CSS_DISPLAY_RUN_IN:
            display->b = type;
            goto listitem;

        case LXB_CSS_DISPLAY_LIST_ITEM:
            display->b = type;
            goto outside;

        default:
            return lxb_css_parser_failed(parser);
    }

listitem_only:

    lxb_css_property_state_get_type(parser, token, type);

    switch (type) {
        /* <display-outside> */
        case LXB_CSS_DISPLAY_BLOCK:
        case LXB_CSS_DISPLAY_INLINE:
        case LXB_CSS_DISPLAY_RUN_IN:
            display->b = type;
            break;

        /* <display-listitem> */
        case LXB_CSS_DISPLAY_FLOW:
        case LXB_CSS_DISPLAY_FLOW_ROOT:
            display->b = type;
            goto outside;

        default:
            return lxb_css_parser_failed(parser);
    }

flow_only:

    lxb_css_property_state_get_type(parser, token, type);

    switch (type) {
        /* <display-listitem> */
        case LXB_CSS_DISPLAY_FLOW:
        case LXB_CSS_DISPLAY_FLOW_ROOT:
            display->c = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

done:

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_order(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    res = lxb_css_property_state_integer(parser, token,
                                         &declar->u.order->integer);
    if (res) {
        declar->u.order->type = LXB_CSS_ORDER__INTEGER;

        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            declar->u.order->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_visibility(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_VISIBILITY_VISIBLE:
        case LXB_CSS_VISIBILITY_HIDDEN:
        case LXB_CSS_VISIBILITY_COLLAPSE:
            declar->u.visibility->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_width(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_VALUE_INITIAL:
            case LXB_CSS_VALUE_INHERIT:
            case LXB_CSS_VALUE_UNSET:
            case LXB_CSS_VALUE_REVERT:
            case LXB_CSS_VALUE_AUTO:
            case LXB_CSS_VALUE_MIN_CONTENT:
            case LXB_CSS_VALUE_MAX_CONTENT:
                declar->u.width->type = type;
                break;

            default:
                return lxb_css_parser_failed(parser);
        }

        lxb_css_syntax_parser_consume(parser);

        return lxb_css_parser_success(parser);
    }

    if (!lxb_css_property_state_length_percentage(parser, token,
                                                  declar->u.user))
    {
        return lxb_css_parser_failed(parser);
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_height(lxb_css_parser_t *parser,
                              const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_width(parser, token, ctx);
}

bool
lxb_css_property_state_box_sizing(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        case LXB_CSS_VALUE_CONTENT_BOX:
        case LXB_CSS_VALUE_BORDER_BOX:
            declar->u.box_sizing->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

static bool
lxb_css_property_state_box_shadow_length(lxb_css_parser_t *parser,
                                         const lxb_css_syntax_token_t *token,
                                         lxb_css_value_length_type_t *length,
                                         bool allow_negative)
{
    lxb_css_value_type_t type;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
            type = LXB_CSS_VALUE__LENGTH;
            break;

        case LXB_CSS_SYNTAX_TOKEN_NUMBER:
            type = LXB_CSS_VALUE__NUMBER;
            break;

        default:
            return false;
    }

    if (!lxb_css_property_state_length(parser, token, &length->length)) {
        return false;
    }

    if (!allow_negative && length->length.num < 0) {
        return false;
    }

    length->type = type;

    return true;
}

bool
lxb_css_property_state_box_shadow(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    bool inset;
    lxb_status_t status;
    unsigned length_count;
    lxb_css_value_type_t type;
    lxb_css_value_length_type_t lengths[4];
    lxb_css_property_box_shadow_t shadow = {0};
    lxb_css_property_box_shadow_layer_t layer = {0};
    lxb_css_rule_declaration_t *declar = ctx;

    static const lexbor_str_t str_inset = lexbor_str("inset");

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);

        if (lxb_css_property_state_is_global(type)
            || type == LXB_CSS_VALUE_NONE)
        {
            shadow.type = type;

            lxb_css_syntax_parser_consume(parser);
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);

            if (token->type != LXB_CSS_SYNTAX_TOKEN__END) {
                return lxb_css_parser_failed(parser);
            }

            *((lxb_css_property_box_shadow_t *) declar->u.user) = shadow;
            return lxb_css_parser_success(parser);
        }
    }

    inset = false;
    length_count = 0;
    shadow.type = LXB_CSS_BOX_SHADOW__LENGTH;

    do {
        inset = false;
        length_count = 0;
        memset(&layer, 0, sizeof(layer));
        layer.blur_radius.type = LXB_CSS_VALUE__UNDEF;
        layer.spread_radius.type = LXB_CSS_VALUE__UNDEF;
        layer.color.type = LXB_CSS_VALUE__UNDEF;

        while (token->type != LXB_CSS_SYNTAX_TOKEN__END
               && token->type != LXB_CSS_SYNTAX_TOKEN_COMMA)
        {
        if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT
            && lxb_css_syntax_token_ident(token)->length == str_inset.length
            && lexbor_str_data_ncasecmp(lxb_css_syntax_token_ident(token)->data,
                                        str_inset.data, str_inset.length))
        {
            if (inset) {
                return lxb_css_parser_failed(parser);
            }

            inset = true;
            lxb_css_syntax_parser_consume(parser);
        }
        else {
            if (layer.color.type == LXB_CSS_VALUE__UNDEF) {
                if (lxb_css_property_state_color_handler(parser, token,
                                                         &layer.color,
                                                         &status))
                {
                    goto next;
                }

                if (status != LXB_STATUS_OK) {
                    return lxb_css_parser_failed(parser);
                }
            }

            if (length_count == 4) {
                return lxb_css_parser_failed(parser);
            }

            if (!lxb_css_property_state_box_shadow_length(parser, token,
                                      &lengths[length_count],
                                      length_count != 2))
            {
                return lxb_css_parser_failed(parser);
            }

            length_count++;
        }

next:

        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
        }

        if (length_count < 2) {
            return lxb_css_parser_failed(parser);
        }

        if (shadow.layer_count >= LXB_CSS_BOX_SHADOW_LAYER_MAX) {
            return lxb_css_parser_failed(parser);
        }

        layer.inset = inset;
        layer.offset_x = lengths[0];
        layer.offset_y = lengths[1];

        if (length_count > 2) {
            layer.blur_radius = lengths[2];
        }

        if (length_count > 3) {
            layer.spread_radius = lengths[3];
        }

        shadow.layers[shadow.layer_count++] = layer;

        if (token->type == LXB_CSS_SYNTAX_TOKEN_COMMA) {
            lxb_css_syntax_parser_consume(parser);
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);
            if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
                return lxb_css_parser_failed(parser);
            }
        }
    }
    while (token->type != LXB_CSS_SYNTAX_TOKEN__END);

    if (shadow.layer_count != 0) {
        shadow.layer = shadow.layers[0];
    }

    *((lxb_css_property_box_shadow_t *) declar->u.user) = shadow;

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_min_width(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_width(parser, token, ctx);
}

bool
lxb_css_property_state_min_height(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_width(parser, token, ctx);
}

bool
lxb_css_property_state_max_width(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_VALUE_INITIAL:
            case LXB_CSS_VALUE_INHERIT:
            case LXB_CSS_VALUE_UNSET:
            case LXB_CSS_VALUE_REVERT:
            case LXB_CSS_VALUE_NONE:
            case LXB_CSS_VALUE_MIN_CONTENT:
            case LXB_CSS_VALUE_MAX_CONTENT:
                declar->u.width->type = type;
                break;

            default:
                return lxb_css_parser_failed(parser);
        }

        lxb_css_syntax_parser_consume(parser);

        return lxb_css_parser_success(parser);
    }

    if (!lxb_css_property_state_length_percentage(parser, token,
                                                  declar->u.user))
    {
        return lxb_css_parser_failed(parser);
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_max_height(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_max_width(parser, token, ctx);
}

static bool
lxb_css_property_state_mp(lxb_css_parser_t *parser,
                          const lxb_css_syntax_token_t *token,
                          lxb_css_rule_declaration_t *declar, bool with_auto)
{
    unsigned int state;
    lxb_css_value_type_t type;
    lxb_css_property_margin_top_t *top;

    state = 1;

next:

    switch (state) {
        case 1:
            top = &declar->u.margin->top;
            break;

        case 2:
            top = &declar->u.margin->right;
            break;

        case 3:
            top = &declar->u.margin->bottom;
            break;

        case 4:
            top = &declar->u.margin->left;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_VALUE_INITIAL:
            case LXB_CSS_VALUE_INHERIT:
            case LXB_CSS_VALUE_UNSET:
            case LXB_CSS_VALUE_REVERT:
                top->type = type;
                break;

            case LXB_CSS_VALUE_AUTO:
                if (with_auto) {
                    top->type = type;
                    break;
                }

                /* Fall through. */

            default:
                return lxb_css_parser_failed(parser);
        }

        lxb_css_syntax_parser_consume(parser);
    }
    else if (!lxb_css_property_state_length_percentage(parser, token,
                                    (lxb_css_value_length_percentage_t *) top))
    {
        return lxb_css_parser_failed(parser);
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
        return lxb_css_parser_success(parser);
    }

    state++;

    goto next;
}

static bool
lxb_css_property_state_mp_top(lxb_css_parser_t *parser,
                              const lxb_css_syntax_token_t *token,
                              lxb_css_rule_declaration_t *declar, bool with_auto)
{
    lxb_css_value_type_t type;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_VALUE_INITIAL:
            case LXB_CSS_VALUE_INHERIT:
            case LXB_CSS_VALUE_UNSET:
            case LXB_CSS_VALUE_REVERT:
                declar->u.margin_top->type = type;
                break;

            case LXB_CSS_VALUE_AUTO:
                if (with_auto) {
                    declar->u.margin_top->type = type;
                    break;
                }

                /* Fall through. */

            default:
                return lxb_css_parser_failed(parser);
        }

        lxb_css_syntax_parser_consume(parser);

        return lxb_css_parser_success(parser);
    }

    if (!lxb_css_property_state_length_percentage(parser, token,
                                                  declar->u.user))
    {
        return lxb_css_parser_failed(parser);
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_margin(lxb_css_parser_t *parser,
                              const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp(parser, token, ctx, true);
}

bool
lxb_css_property_state_margin_top(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp_top(parser, token, ctx, true);
}

bool
lxb_css_property_state_margin_right(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp_top(parser, token, ctx, true);
}

bool
lxb_css_property_state_margin_bottom(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp_top(parser, token, ctx, true);
}

bool
lxb_css_property_state_margin_left(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp_top(parser, token, ctx, true);
}

bool
lxb_css_property_state_padding(lxb_css_parser_t *parser,
                               const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp(parser, token, ctx, false);
}

bool
lxb_css_property_state_padding_top(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp_top(parser, token, ctx, false);
}

bool
lxb_css_property_state_padding_right(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp_top(parser, token, ctx, false);
}

bool
lxb_css_property_state_padding_bottom(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp_top(parser, token, ctx, false);
}

bool
lxb_css_property_state_padding_left(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp_top(parser, token, ctx, false);
}

static bool
lxb_css_property_state_is_global(lxb_css_value_type_t type)
{
    switch (type) {
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            return true;

        default:
            return false;
    }
}

static bool
lxb_css_property_state_gap_value(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token,
                                 lxb_css_value_length_percentage_t *value,
                                 bool allow_global)
{
    lxb_css_value_type_t type;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);

        if (type == LXB_CSS_VALUE_NORMAL
            || (allow_global && lxb_css_property_state_is_global(type)))
        {
            value->type = type;
            lxb_css_syntax_parser_consume(parser);
            return true;
        }

        return false;
    }

    return lxb_css_property_state_length_percentage(parser, token, value);
}

bool
lxb_css_property_state_gap(lxb_css_parser_t *parser,
                           const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;

    if (!lxb_css_property_state_gap_value(parser, token,
                                          &declar->u.gap->row, true))
    {
        return lxb_css_parser_failed(parser);
    }

    if (lxb_css_property_state_is_global(declar->u.gap->row.type)) {
        declar->u.gap->column.type = declar->u.gap->row.type;
        return lxb_css_parser_success(parser);
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
        declar->u.gap->column = declar->u.gap->row;
        return lxb_css_parser_success(parser);
    }

    if (!lxb_css_property_state_gap_value(parser, token,
                                          &declar->u.gap->column, false))
    {
        return lxb_css_parser_failed(parser);
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN__END) {
        return lxb_css_parser_failed(parser);
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_row_gap(lxb_css_parser_t *parser,
                               const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;

    return lxb_css_property_state_gap_value(parser, token,
                                            declar->u.row_gap, true)
        ? lxb_css_parser_success(parser)
        : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_column_gap(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;

    return lxb_css_property_state_gap_value(parser, token,
                                            declar->u.column_gap, true)
        ? lxb_css_parser_success(parser)
        : lxb_css_parser_failed(parser);
}

static bool
lxb_css_property_state_line_width_style_color(lxb_css_parser_t *parser,
                                              const lxb_css_syntax_token_t *token,
                                              lxb_css_property_border_t *border)
{
    lxb_status_t status;
    lxb_css_value_type_t type;
    const lxb_css_data_t *unit;
    lxb_css_value_length_t *length;
    lxb_css_syntax_token_string_t *str;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
            if (border->width.type != LXB_CSS_VALUE__UNDEF) {
                return false;
            }

            str = &lxb_css_syntax_token_dimension(token)->str;

            unit = lxb_css_unit_absolute_relative_by_name(str->data,
                                                          str->length);
            if (unit == NULL) {
                return false;
            }

            length = &border->width.length;

            border->width.type = LXB_CSS_VALUE__LENGTH;
            length->num = lxb_css_syntax_token_dimension(token)->num.num;
            length->is_float = lxb_css_syntax_token_dimension(token)->num.is_float;
            length->unit = (lxb_css_unit_t) unit->unique;
            break;

        case LXB_CSS_SYNTAX_TOKEN_NUMBER:
            if (border->width.type != LXB_CSS_VALUE__UNDEF) {
                return false;
            }

            length = &border->width.length;

            border->width.type = LXB_CSS_VALUE__NUMBER;
            length->num = lxb_css_syntax_token_number(token)->num;
            length->is_float = lxb_css_syntax_token_number(token)->is_float;
            break;

        case LXB_CSS_SYNTAX_TOKEN_IDENT:
            type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                         lxb_css_syntax_token_ident(token)->length);
            switch (type) {
                case LXB_CSS_VALUE_THIN:
                case LXB_CSS_VALUE_MEDIUM:
                case LXB_CSS_VALUE_THICK:
                    if (border->width.type != LXB_CSS_VALUE__UNDEF) {
                        return false;
                    }

                    border->width.type = type;
                    break;

                case LXB_CSS_VALUE_NONE:
                case LXB_CSS_VALUE_HIDDEN:
                case LXB_CSS_VALUE_DOTTED:
                case LXB_CSS_VALUE_DASHED:
                case LXB_CSS_VALUE_SOLID:
                case LXB_CSS_VALUE_DOUBLE:
                case LXB_CSS_VALUE_GROOVE:
                case LXB_CSS_VALUE_RIDGE:
                case LXB_CSS_VALUE_INSET:
                case LXB_CSS_VALUE_OUTSET:
                    if (border->style != LXB_CSS_VALUE__UNDEF) {
                        return false;
                    }

                    border->style = type;
                    break;

                default:
                    goto color;
            }

            break;

        default:
            goto color;
    }

    lxb_css_syntax_parser_consume(parser);

    return true;

color:

    if (border->color.type != LXB_CSS_VALUE__UNDEF) {
        return false;
    }

    return lxb_css_property_state_color_handler(parser, token, &border->color,
                                                &status);
}

bool
lxb_css_property_state_border(lxb_css_parser_t *parser,
                              const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_VALUE_INITIAL:
            case LXB_CSS_VALUE_INHERIT:
            case LXB_CSS_VALUE_UNSET:
            case LXB_CSS_VALUE_REVERT:
                declar->u.border->style = type;

                lxb_css_syntax_parser_consume(parser);
                return lxb_css_parser_success(parser);

            default:
                break;
        }
    }

    res = lxb_css_property_state_line_width_style_color(parser, token,
                                                        declar->u.border);
    if (!res) {
        return lxb_css_parser_failed(parser);
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
        return lxb_css_parser_success(parser);
    }

    res = lxb_css_property_state_line_width_style_color(parser, token,
                                                        declar->u.border);
    if (!res) {
        return lxb_css_parser_failed(parser);
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
        return lxb_css_parser_success(parser);
    }

    res = lxb_css_property_state_line_width_style_color(parser, token,
                                                        declar->u.border);
    if (!res) {
        return lxb_css_parser_failed(parser);
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_border_top(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_border(parser, token, ctx);
}

bool
lxb_css_property_state_border_right(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_border(parser, token, ctx);
}

bool
lxb_css_property_state_border_bottom(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_border(parser, token, ctx);
}

bool
lxb_css_property_state_border_left(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_border(parser, token, ctx);
}

static void
lxb_css_property_state_border_color_apply(lxb_css_property_border_color_t *border,
                                          lxb_css_value_color_t values[4],
                                          unsigned count)
{
    border->top = values[0];
    border->right = (count > 1) ? values[1] : values[0];
    border->bottom = (count > 2) ? values[2] : values[0];
    border->left = (count > 3) ? values[3] : border->right;
}

static void
lxb_css_property_state_border_color_set_global(
    lxb_css_property_border_color_t *border, lxb_css_value_type_t type)
{
    border->top.type = type;
    border->right.type = type;
    border->bottom.type = type;
    border->left.type = type;
}

bool
lxb_css_property_state_border_color(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_status_t status;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_value_color_t values[4] = {{0}, {0}, {0}, {0}};
    unsigned count = 0;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);

        if (lxb_css_property_state_is_global(type)) {
            lxb_css_property_state_border_color_set_global(
                declar->u.border_color, type);

            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }
    }

    do {
        if (count == 4) {
            return lxb_css_parser_failed(parser);
        }

        if (!lxb_css_property_state_color_handler(parser, token,
                                                  &values[count], &status))
        {
            return lxb_css_parser_failed(parser);
        }

        count++;

        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }
    while (token->type != LXB_CSS_SYNTAX_TOKEN__END);

    lxb_css_property_state_border_color_apply(declar->u.border_color,
                                              values, count);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_border_top_color(lxb_css_parser_t *parser,
                                        const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_color(parser, token, ctx);
}

bool
lxb_css_property_state_border_right_color(lxb_css_parser_t *parser,
                                          const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_color(parser, token, ctx);
}

bool
lxb_css_property_state_border_bottom_color(lxb_css_parser_t *parser,
                                           const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_color(parser, token, ctx);
}

bool
lxb_css_property_state_border_left_color(lxb_css_parser_t *parser,
                                         const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_color(parser, token, ctx);
}

/* Helper: parse a single <line-width> (thin|medium|thick|<length>) into
 * a lxb_css_value_length_type_t.  Returns true on success. */
static bool
lxb_css_property_state_line_width_one(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token,
                                      lxb_css_value_length_type_t *out)
{
    lxb_css_value_type_t type;
    const lxb_css_data_t *unit;
    lxb_css_syntax_token_string_t *str;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
            str = &lxb_css_syntax_token_dimension(token)->str;
            unit = lxb_css_unit_absolute_relative_by_name(str->data, str->length);
            if (unit == NULL) {
                return false;
            }
            out->type = LXB_CSS_VALUE__LENGTH;
            out->length.num = lxb_css_syntax_token_dimension(token)->num.num;
            out->length.is_float = lxb_css_syntax_token_dimension(token)->num.is_float;
            out->length.unit = (lxb_css_unit_t) unit->unique;
            lxb_css_syntax_parser_consume(parser);
            return true;

        case LXB_CSS_SYNTAX_TOKEN_NUMBER:
            out->type = LXB_CSS_VALUE__NUMBER;
            out->length.num = lxb_css_syntax_token_number(token)->num;
            out->length.is_float = lxb_css_syntax_token_number(token)->is_float;
            out->length.unit = LXB_CSS_UNIT__UNDEF;
            lxb_css_syntax_parser_consume(parser);
            return true;

        case LXB_CSS_SYNTAX_TOKEN_IDENT:
            type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                         lxb_css_syntax_token_ident(token)->length);
            switch (type) {
                case LXB_CSS_VALUE_THIN:
                case LXB_CSS_VALUE_MEDIUM:
                case LXB_CSS_VALUE_THICK:
                    out->type = type;
                    lxb_css_syntax_parser_consume(parser);
                    return true;
                default:
                    break;
            }
            break;

        default:
            break;
    }

    return false;
}

/* Helper: apply 1-4 line-width values to top/right/bottom/left. */
static void
lxb_css_property_state_border_width_apply(lxb_css_property_border_width_t *bw,
                                          lxb_css_value_length_type_t vals[4],
                                          unsigned count)
{
    bw->top    = vals[0];
    bw->right  = (count > 1) ? vals[1] : vals[0];
    bw->bottom = (count > 2) ? vals[2] : vals[0];
    bw->left   = (count > 3) ? vals[3] : bw->right;
}

bool
lxb_css_property_state_border_style(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_border_style_t *bs = declar->u.border_style;
    lxb_css_value_type_t vals[4] = {
        LXB_CSS_VALUE__UNDEF, LXB_CSS_VALUE__UNDEF,
        LXB_CSS_VALUE__UNDEF, LXB_CSS_VALUE__UNDEF
    };
    unsigned count = 0;

    /* Handle global keywords. */
    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        if (lxb_css_property_state_is_global(type)) {
            bs->top = bs->right = bs->bottom = bs->left = type;
            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }
    }

    do {
        if (count == 4 || token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
            return lxb_css_parser_failed(parser);
        }

        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_VALUE_NONE:
            case LXB_CSS_VALUE_HIDDEN:
            case LXB_CSS_VALUE_DOTTED:
            case LXB_CSS_VALUE_DASHED:
            case LXB_CSS_VALUE_SOLID:
            case LXB_CSS_VALUE_DOUBLE:
            case LXB_CSS_VALUE_GROOVE:
            case LXB_CSS_VALUE_RIDGE:
            case LXB_CSS_VALUE_INSET:
            case LXB_CSS_VALUE_OUTSET:
                vals[count++] = type;
                lxb_css_syntax_parser_consume(parser);
                break;
            default:
                return lxb_css_parser_failed(parser);
        }

        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }
    while (token->type != LXB_CSS_SYNTAX_TOKEN__END);

    bs->top    = vals[0];
    bs->right  = (count > 1) ? vals[1] : vals[0];
    bs->bottom = (count > 2) ? vals[2] : vals[0];
    bs->left   = (count > 3) ? vals[3] : bs->right;

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_border_width(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_border_width_t *bw = declar->u.border_width;
    lxb_css_value_length_type_t vals[4] = {
        {LXB_CSS_VALUE__UNDEF, {0}}, {LXB_CSS_VALUE__UNDEF, {0}},
        {LXB_CSS_VALUE__UNDEF, {0}}, {LXB_CSS_VALUE__UNDEF, {0}}
    };
    unsigned count = 0;

    /* Handle global keywords. */
    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        if (lxb_css_property_state_is_global(type)) {
            bw->top.type = bw->right.type = bw->bottom.type = bw->left.type = type;
            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }
    }

    do {
        if (count == 4) {
            return lxb_css_parser_failed(parser);
        }

        if (!lxb_css_property_state_line_width_one(parser, token, &vals[count])) {
            return lxb_css_parser_failed(parser);
        }
        count++;

        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }
    while (token->type != LXB_CSS_SYNTAX_TOKEN__END);

    lxb_css_property_state_border_width_apply(bw, vals, count);
    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_border_top_width(lxb_css_parser_t *parser,
                                        const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_value_length_type_t *out = declar->u.border_top_width;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        if (lxb_css_property_state_is_global(type)) {
            out->type = type;
            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }
    }

    return lxb_css_property_state_line_width_one(parser, token, out)
         ? lxb_css_parser_success(parser)
         : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_border_right_width(lxb_css_parser_t *parser,
                                          const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_value_length_type_t *out = declar->u.border_right_width;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        if (lxb_css_property_state_is_global(type)) {
            out->type = type;
            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }
    }

    return lxb_css_property_state_line_width_one(parser, token, out)
         ? lxb_css_parser_success(parser)
         : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_border_bottom_width(lxb_css_parser_t *parser,
                                           const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_value_length_type_t *out = declar->u.border_bottom_width;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        if (lxb_css_property_state_is_global(type)) {
            out->type = type;
            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }
    }

    return lxb_css_property_state_line_width_one(parser, token, out)
         ? lxb_css_parser_success(parser)
         : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_border_left_width(lxb_css_parser_t *parser,
                                         const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_value_length_type_t *out = declar->u.border_left_width;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        if (lxb_css_property_state_is_global(type)) {
            out->type = type;
            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }
    }

    return lxb_css_property_state_line_width_one(parser, token, out)
         ? lxb_css_parser_success(parser)
         : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_border_collapse(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_border_collapse_t *out = declar->u.border_collapse;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);

    /* Accept: collapse, separate, or a global keyword. */
    switch (type) {
        case LXB_CSS_VALUE_COLLAPSE:
            *out = type;
            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        default:
            break;
    }

    /* "separate" is not in the value enum; match it by name. */
    {
        const lxb_css_syntax_token_ident_t *ident = lxb_css_syntax_token_ident(token);
        static const lxb_char_t kw_separate[] = "separate";
        if (ident->length == 8 &&
            lexbor_str_data_ncasecmp(ident->data, kw_separate, 8)) {
            *out = LXB_CSS_VALUE__UNDEF;  /* treat as "not collapse" */
            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }
    }

    if (lxb_css_property_state_is_global(type)) {
        *out = type;
        lxb_css_syntax_parser_consume(parser);
        return lxb_css_parser_success(parser);
    }

    return lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_list_style_type(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_list_style_type_t *out = declar->u.list_style_type;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    if (lxb_css_property_state_is_global(type)) {
        out->type = type;
        lxb_css_syntax_parser_consume(parser);
        return lxb_css_parser_success(parser);
    }

    const lxb_css_syntax_token_ident_t *ident = lxb_css_syntax_token_ident(token);
    if (ident->length == 4 &&
        lexbor_str_data_ncasecmp(ident->data, (const lxb_char_t *) "disc", 4)) {
        out->type = LXB_CSS_LIST_STYLE_TYPE_DISC;
    }
    else if (ident->length == 6 &&
        lexbor_str_data_ncasecmp(ident->data, (const lxb_char_t *) "circle", 6)) {
        out->type = LXB_CSS_LIST_STYLE_TYPE_CIRCLE;
    }
    else if (ident->length == 6 &&
        lexbor_str_data_ncasecmp(ident->data, (const lxb_char_t *) "square", 6)) {
        out->type = LXB_CSS_LIST_STYLE_TYPE_SQUARE;
    }
    else if (ident->length == 7 &&
        lexbor_str_data_ncasecmp(ident->data, (const lxb_char_t *) "decimal", 7)) {
        out->type = LXB_CSS_LIST_STYLE_TYPE_DECIMAL;
    }
    else if (type == LXB_CSS_VALUE_NONE) {
        out->type = LXB_CSS_LIST_STYLE_TYPE_NONE;
    }
    else {
        return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);
    return lxb_css_parser_success(parser);
}

static void
lxb_css_property_state_border_radius_set_global(
    lxb_css_property_border_radius_corner_t *corner, lxb_css_value_type_t type)
{
    corner->h.type = type;
    corner->v.type = type;
}

static bool
lxb_css_property_state_border_radius_end(lxb_css_parser_t *parser)
{
    const lxb_css_syntax_token_t *token;

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    return token->type == LXB_CSS_SYNTAX_TOKEN__END;
}

static bool
lxb_css_property_state_border_radius_corner(
    lxb_css_parser_t *parser, const lxb_css_syntax_token_t *token,
    lxb_css_property_border_radius_corner_t *corner)
{
    lxb_css_value_type_t type;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);

        if (!lxb_css_property_state_is_global(type)) {
            return false;
        }

        lxb_css_property_state_border_radius_set_global(corner, type);
        lxb_css_syntax_parser_consume(parser);

        return lxb_css_property_state_border_radius_end(parser);
    }

    if (!lxb_css_property_state_length_percentage(parser, token, &corner->h)) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
        corner->v = corner->h;
        return true;
    }

    if (!lxb_css_property_state_length_percentage(parser, token, &corner->v)) {
        return false;
    }

    return lxb_css_property_state_border_radius_end(parser);
}

static const lxb_css_value_length_percentage_t *
lxb_css_property_state_border_radius_value(
    const lxb_css_value_length_percentage_t values[4], unsigned count,
    unsigned index)
{
    switch (count) {
        case 1:
            return &values[0];

        case 2:
            return (index == 0 || index == 2) ? &values[0] : &values[1];

        case 3:
            return (index == 0) ? &values[0]
                 : (index == 2) ? &values[2]
                                : &values[1];

        default:
            return &values[index];
    }
}

static bool
lxb_css_property_state_border_radius_list(
    lxb_css_parser_t *parser, const lxb_css_syntax_token_t *token,
    lxb_css_value_length_percentage_t values[4], unsigned *count,
    bool allow_slash, bool *slash)
{
    *count = 0;
    *slash = false;

    while (true) {
        if (*count == 4) {
            return false;
        }

        if (!lxb_css_property_state_length_percentage(parser, token,
                                                      &values[*count]))
        {
            return false;
        }

        (*count)++;

        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
            return true;
        }

        if (token->type == LXB_CSS_SYNTAX_TOKEN_DELIM
            && lxb_css_syntax_token_delim(token)->character == '/')
        {
            if (!allow_slash) {
                return false;
            }

            lxb_css_syntax_parser_consume(parser);
            *slash = true;

            return true;
        }
    }
}

static void
lxb_css_property_state_border_radius_assign(
    lxb_css_property_border_radius_t *radius,
    const lxb_css_value_length_percentage_t h[4], unsigned h_count,
    const lxb_css_value_length_percentage_t v[4], unsigned v_count)
{
    radius->top_left.h =
        *lxb_css_property_state_border_radius_value(h, h_count, 0);
    radius->top_right.h =
        *lxb_css_property_state_border_radius_value(h, h_count, 1);
    radius->bottom_right.h =
        *lxb_css_property_state_border_radius_value(h, h_count, 2);
    radius->bottom_left.h =
        *lxb_css_property_state_border_radius_value(h, h_count, 3);

    radius->top_left.v =
        *lxb_css_property_state_border_radius_value(v, v_count, 0);
    radius->top_right.v =
        *lxb_css_property_state_border_radius_value(v, v_count, 1);
    radius->bottom_right.v =
        *lxb_css_property_state_border_radius_value(v, v_count, 2);
    radius->bottom_left.v =
        *lxb_css_property_state_border_radius_value(v, v_count, 3);
}

bool
lxb_css_property_state_border_radius(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token,
                                     void *ctx)
{
    bool slash;
    unsigned h_count, v_count;
    lxb_css_value_type_t type;
    lxb_css_value_length_percentage_t h[4];
    lxb_css_value_length_percentage_t v[4];
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);

        if (!lxb_css_property_state_is_global(type)) {
            return lxb_css_parser_failed(parser);
        }

        lxb_css_property_state_border_radius_set_global(
            &declar->u.border_radius->top_left, type);
        lxb_css_property_state_border_radius_set_global(
            &declar->u.border_radius->top_right, type);
        lxb_css_property_state_border_radius_set_global(
            &declar->u.border_radius->bottom_right, type);
        lxb_css_property_state_border_radius_set_global(
            &declar->u.border_radius->bottom_left, type);

        lxb_css_syntax_parser_consume(parser);

        return lxb_css_property_state_border_radius_end(parser)
            ? lxb_css_parser_success(parser)
            : lxb_css_parser_failed(parser);
    }

    if (!lxb_css_property_state_border_radius_list(parser, token, h,
                                                  &h_count, true, &slash))
    {
        return lxb_css_parser_failed(parser);
    }

    if (slash) {
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        if (!lxb_css_property_state_border_radius_list(parser, token, v,
                                                      &v_count, false, &slash))
        {
            return lxb_css_parser_failed(parser);
        }
    }
    else {
        for (unsigned i = 0; i < h_count; i++) {
            v[i] = h[i];
        }

        v_count = h_count;
    }

    lxb_css_property_state_border_radius_assign(declar->u.border_radius, h,
                                                h_count, v, v_count);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_border_top_left_radius(lxb_css_parser_t *parser,
                                              const lxb_css_syntax_token_t *token,
                                              void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;

    return lxb_css_property_state_border_radius_corner(
        parser, token, declar->u.border_top_left_radius)
        ? lxb_css_parser_success(parser)
        : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_border_top_right_radius(lxb_css_parser_t *parser,
                                               const lxb_css_syntax_token_t *token,
                                               void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;

    return lxb_css_property_state_border_radius_corner(
        parser, token, declar->u.border_top_right_radius)
        ? lxb_css_parser_success(parser)
        : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_border_bottom_right_radius(
    lxb_css_parser_t *parser, const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;

    return lxb_css_property_state_border_radius_corner(
        parser, token, declar->u.border_bottom_right_radius)
        ? lxb_css_parser_success(parser)
        : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_border_bottom_left_radius(
    lxb_css_parser_t *parser, const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;

    return lxb_css_property_state_border_radius_corner(
        parser, token, declar->u.border_bottom_left_radius)
        ? lxb_css_parser_success(parser)
        : lxb_css_parser_failed(parser);
}

/*
 * Parse `to <side-or-corner>` into an angle in degrees.
 * CSS spec: `to right` = 90deg, `to bottom` = 180deg (default),
 * `to left` = 270deg, `to top` = 0deg.
 * Returns false if the ident sequence is not a valid direction.
 */
static bool
lxb_css_property_state_gradient_to_direction(lxb_css_parser_t *parser,
                                             const lxb_css_syntax_token_t *token,
                                             double *angle_deg_out)
{
    lxb_css_value_type_t side;

    /* Already consumed `to`; token is the first side keyword. */
    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return false;
    }

    side = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);

    switch (side) {
        case LXB_CSS_VALUE_TOP:    *angle_deg_out = 0.0;   break;
        case LXB_CSS_VALUE_RIGHT:  *angle_deg_out = 90.0;  break;
        case LXB_CSS_VALUE_BOTTOM: *angle_deg_out = 180.0; break;
        case LXB_CSS_VALUE_LEFT:   *angle_deg_out = 270.0; break;
        default:
            return false;
    }

    lxb_css_syntax_parser_consume(parser);
    return true;
}

/*
 * Skip all remaining tokens until and including the closing `)`.
 * Used to consume the remainder of an unrecognised function call.
 */
static void
lxb_css_property_state_skip_to_r_paren(lxb_css_parser_t *parser)
{
    const lxb_css_syntax_token_t *tok;

    for (;;) {
        tok = lxb_css_syntax_parser_token(parser);
        if (tok == NULL) break;
        if (tok->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS
            || tok->type == LXB_CSS_SYNTAX_TOKEN__END)
        {
            lxb_css_syntax_parser_consume(parser);
            break;
        }
        lxb_css_syntax_parser_consume(parser);
    }
}

static bool
lxb_css_property_state_is_gradient_stop_position(
    const lxb_css_syntax_token_t *token)
{
    if (token == NULL) return false;

    switch (token->type) {
        case LXB_CSS_SYNTAX_TOKEN_DIMENSION:
        case LXB_CSS_SYNTAX_TOKEN_NUMBER:
        case LXB_CSS_SYNTAX_TOKEN_PERCENTAGE:
            return true;

        default:
            return false;
    }
}

static bool
lxb_css_property_state_ident_is(const lxb_css_syntax_token_t *token,
                                const char *name, size_t length)
{
    return token != NULL
        && token->type == LXB_CSS_SYNTAX_TOKEN_IDENT
        && lxb_css_syntax_token_ident(token)->length == length
        && lexbor_str_data_ncasecmp(lxb_css_syntax_token_ident(token)->data,
                                    (const lxb_char_t *) name, length);
}

static bool
lxb_css_property_state_take_gradient_stop_pct(lxb_css_parser_t *parser,
                                              double *pct_out)
{
    const lxb_css_syntax_token_t *token;
    bool has_pct = false;

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (!lxb_css_property_state_is_gradient_stop_position(token)) {
        return false;
    }

    if (token->type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE) {
        *pct_out = lxb_css_syntax_token_percentage(token)->num;
        has_pct = true;
    }

    lxb_css_syntax_parser_consume(parser);

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (lxb_css_property_state_is_gradient_stop_position(token)) {
        lxb_css_syntax_parser_consume(parser);
    }

    return has_pct;
}

static bool
lxb_css_property_state_gradient_position_component(
    lxb_css_parser_t *parser, const lxb_css_syntax_token_t *token,
    double *pct_out)
{
    if (token == NULL) return false;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE) {
        *pct_out = lxb_css_syntax_token_percentage(token)->num;
        lxb_css_syntax_parser_consume(parser);
        return true;
    }

    if (lxb_css_property_state_ident_is(token, "center", 6)) {
        *pct_out = 50.0;
    }
    else if (lxb_css_property_state_ident_is(token, "left", 4)
             || lxb_css_property_state_ident_is(token, "top", 3))
    {
        *pct_out = 0.0;
    }
    else if (lxb_css_property_state_ident_is(token, "right", 5)
             || lxb_css_property_state_ident_is(token, "bottom", 6))
    {
        *pct_out = 100.0;
    }
    else {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);
    return true;
}

static bool
lxb_css_property_state_radial_position(lxb_css_parser_t *parser,
                                       lxb_css_property_gradient_t *gradient)
{
    double x = 50.0, y = 50.0;
    const lxb_css_syntax_token_t *token;

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (!lxb_css_property_state_ident_is(token, "at", 2)) {
        return true;
    }

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (!lxb_css_property_state_gradient_position_component(parser, token, &x)) {
        return false;
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (token != NULL && token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
        if (!lxb_css_property_state_gradient_position_component(parser, token, &y)) {
            return false;
        }
    }

    gradient->center_x_pct = x;
    gradient->center_y_pct = y;
    return true;
}

static bool
lxb_css_property_state_radial_prelude_start(
    const lxb_css_syntax_token_t *token)
{
    if (token == NULL) return false;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE
        || token->type == LXB_CSS_SYNTAX_TOKEN_DIMENSION
        || token->type == LXB_CSS_SYNTAX_TOKEN_NUMBER)
    {
        return true;
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return false;
    }

    return lxb_css_property_state_ident_is(token, "at", 2)
        || lxb_css_property_state_ident_is(token, "circle", 6)
        || lxb_css_property_state_ident_is(token, "ellipse", 7)
        || lxb_css_property_state_ident_is(token, "closest-side", 12)
        || lxb_css_property_state_ident_is(token, "closest-corner", 14)
        || lxb_css_property_state_ident_is(token, "farthest-side", 13)
        || lxb_css_property_state_ident_is(token, "farthest-corner", 15);
}

static bool
lxb_css_property_state_radial_prelude(lxb_css_parser_t *parser,
                                      lxb_css_property_gradient_t *gradient)
{
    const lxb_css_syntax_token_t *token;
    double x = 50.0, y = 50.0;
    bool saw_prelude = false;

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (!lxb_css_property_state_radial_prelude_start(token)) {
        return true;
    }

    while (token != NULL && token->type != LXB_CSS_SYNTAX_TOKEN_COMMA
           && token->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS
           && token->type != LXB_CSS_SYNTAX_TOKEN__END)
    {
        if (lxb_css_property_state_ident_is(token, "at", 2)) {
            saw_prelude = true;
            lxb_css_syntax_parser_consume(parser);

            token = lxb_css_syntax_parser_token_wo_ws(parser);
            if (!lxb_css_property_state_gradient_position_component(
                    parser, token, &x))
            {
                return false;
            }

            token = lxb_css_syntax_parser_token_wo_ws(parser);
            if (token != NULL && token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
                if (!lxb_css_property_state_gradient_position_component(
                        parser, token, &y))
                {
                    return false;
                }
            }

            gradient->center_x_pct = x;
            gradient->center_y_pct = y;
        }
        else if (lxb_css_property_state_radial_prelude_start(token)) {
            saw_prelude = true;
            lxb_css_syntax_parser_consume(parser);
        }
        else {
            return false;
        }

        token = lxb_css_syntax_parser_token_wo_ws(parser);
    }

    if (saw_prelude) {
        if (token == NULL || token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
            return false;
        }
        lxb_css_syntax_parser_consume(parser);
    }

    return true;
}

/*
 * Parse the interior of linear-gradient() or radial-gradient().
 *
 * Supported subset (2-stop only — NanoVG is natively 2-stop):
 *
 *   linear-gradient([ <angle> | to <side> ]?, <color>, <color>)
 *   radial-gradient([ circle ]?, <color>, <color>)
 *
 * The `gradient` struct is populated on success and true is returned.
 * On any parse error false is returned; the caller decides whether to
 * fail the whole declaration.
 *
 * The token argument is the first token AFTER the opening `(` has been
 * consumed by the tokeniser (i.e. lxb_css_syntax_parser_consume(parser)
 * was called on the FUNCTION token by the caller).
 */
static bool
lxb_css_property_state_gradient_args(lxb_css_parser_t *parser,
                                     lxb_css_gradient_kind_t kind,
                                     lxb_css_property_gradient_t *gradient)
{
    lxb_status_t status;
    const lxb_css_syntax_token_t *token;

    gradient->kind      = kind;
    gradient->angle_deg = 180.0; /* CSS default: `to bottom` */
    gradient->center_x_pct = 50.0;
    gradient->center_y_pct = 50.0;
    gradient->stop0_pos_pct = 0.0;
    gradient->stop1_pos_pct = 100.0;
    gradient->has_stop0_pos_pct = false;
    gradient->has_stop1_pos_pct = false;
    gradient->stop0.type = LXB_CSS_VALUE__UNDEF;
    gradient->stop1.type = LXB_CSS_VALUE__UNDEF;
    gradient->stop_count = 0;
    memset(gradient->stops, 0, sizeof(gradient->stops));

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (token == NULL) return false;

    if (kind == LXB_CSS_GRADIENT_LINEAR) {
        /* Optional: <angle> or `to <side>` */
        if (token->type == LXB_CSS_SYNTAX_TOKEN_DIMENSION) {
            /* <angle>: convert to degrees */
            const lxb_css_data_t *unit =
                lxb_css_unit_angel_by_name(
                    lxb_css_syntax_token_dimension(token)->str.data,
                    lxb_css_syntax_token_dimension(token)->str.length);
            if (unit != NULL) {
                double num = lxb_css_syntax_token_dimension(token)->num.num;
                switch ((lxb_css_unit_angel_t) unit->unique) {
                    case LXB_CSS_UNIT_DEG:  gradient->angle_deg = num;         break;
                    case LXB_CSS_UNIT_TURN: gradient->angle_deg = num * 360.0; break;
                    case LXB_CSS_UNIT_RAD:  gradient->angle_deg = num * (180.0 / 3.14159265358979323846); break;
                    case LXB_CSS_UNIT_GRAD: gradient->angle_deg = num * 0.9;  break;
                    default:                gradient->angle_deg = num;         break;
                }
                lxb_css_syntax_parser_consume(parser);

                /* Expect comma */
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                if (token == NULL || token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
                    return false;
                }
                lxb_css_syntax_parser_consume(parser);
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                if (token == NULL) return false;
            }
            /* else: unrecognized dimension — fall through and try parsing as color */
        }
        else if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
            /* `to` is not in the CSS value enum table; compare directly. */
            const lxb_char_t *kw  = lxb_css_syntax_token_ident(token)->data;
            size_t            kwl = lxb_css_syntax_token_ident(token)->length;
            if (kwl == 2
                && lexbor_str_data_ncasecmp(kw, (const lxb_char_t *)"to", 2))
            {
                lxb_css_syntax_parser_consume(parser);
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                if (token == NULL) return false;

                if (!lxb_css_property_state_gradient_to_direction(
                        parser, token, &gradient->angle_deg))
                {
                    return false;
                }

                /* Expect comma */
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                if (token == NULL || token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
                    return false;
                }
                lxb_css_syntax_parser_consume(parser);
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                if (token == NULL) return false;
            }
            /* else: treat as color (no direction prefix) */
        }
    }
    else { /* RADIAL */
        if (!lxb_css_property_state_radial_prelude(parser, gradient)) {
            return false;
        }
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        if (token == NULL) return false;

        /* Optional: `circle` keyword */
        if (false && token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
            /* `circle` is not in the CSS value enum table; compare directly. */
            const lxb_char_t *kw  = lxb_css_syntax_token_ident(token)->data;
            size_t            kwl = lxb_css_syntax_token_ident(token)->length;
            if (kwl == 6
                && lexbor_str_data_ncasecmp(kw, (const lxb_char_t *)"circle", 6))
            {
                lxb_css_syntax_parser_consume(parser);
                if (!lxb_css_property_state_radial_position(parser, gradient)) {
                    return false;
                }

                /* Expect comma */
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                if (token == NULL || token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
                    return false;
                }
                lxb_css_syntax_parser_consume(parser);
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                if (token == NULL) return false;
            }
            /* else: may still be a color keyword — fall through */
        }
    }

    /* Parse stop 0. `stop0` (first) and `stop1` (last) are kept as legacy
     * aliases while every parsed stop is also appended to `stops[]`. */
    if (!lxb_css_property_state_color_handler(parser, token,
                                               &gradient->stop0, &status)) {
        return false;
    }
    gradient->has_stop0_pos_pct =
        lxb_css_property_state_take_gradient_stop_pct(
            parser, &gradient->stop0_pos_pct);

    gradient->stops[0].color       = gradient->stop0;
    gradient->stops[0].pos_pct     = gradient->stop0_pos_pct;
    gradient->stops[0].has_pos_pct = gradient->has_stop0_pos_pct;
    gradient->stop_count = 1;

    /* Expect comma */
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (token == NULL || token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
        return false;
    }
    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (token == NULL) return false;

    /* Parse stop 1 (the second, mandatory stop). */
    {
        lxb_css_value_color_t stop_color;
        double stop_pos = 0.0;
        bool   has_pos;

        stop_color.type = LXB_CSS_VALUE__UNDEF;
        if (!lxb_css_property_state_color_handler(parser, token,
                                                   &stop_color, &status)) {
            return false;
        }
        has_pos = lxb_css_property_state_take_gradient_stop_pct(
                      parser, &stop_pos);

        /* stop1 alias = last stop parsed so far. */
        gradient->stop1 = stop_color;
        gradient->stop1_pos_pct = stop_pos;
        gradient->has_stop1_pos_pct = has_pos;
        gradient->stops[1].color       = stop_color;
        gradient->stops[1].pos_pct     = stop_pos;
        gradient->stops[1].has_pos_pct = has_pos;
        gradient->stop_count = 2;
    }

    /* `repeating`-style two-coincident-stop pattern is detected as a
     * stripe tile fill; keep the existing special case for linear. */
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (kind == LXB_CSS_GRADIENT_LINEAR
        && token != NULL && token->type == LXB_CSS_SYNTAX_TOKEN_COMMA
        && gradient->has_stop0_pos_pct && gradient->has_stop1_pos_pct
        && gradient->stop0_pos_pct == gradient->stop1_pos_pct)
    {
        gradient->kind = LXB_CSS_GRADIENT_LINEAR_STRIPES;
        lxb_css_property_state_skip_to_r_paren(parser);
        return true;
    }

    /* Append every remaining `,<color>` stop into the ordered list.
     * `stop1` continues to track the LAST stop for 2-stop consumers. */
    while (token != NULL && token->type == LXB_CSS_SYNTAX_TOKEN_COMMA) {
        lxb_css_value_color_t stop_color;
        double stop_pos = 0.0;
        bool   has_pos;

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        if (token == NULL) return false;

        stop_color.type = LXB_CSS_VALUE__UNDEF;
        if (!lxb_css_property_state_color_handler(parser, token,
                                                   &stop_color, &status)) {
            return false;
        }
        has_pos = lxb_css_property_state_take_gradient_stop_pct(
                      parser, &stop_pos);

        gradient->stop1 = stop_color;
        gradient->stop1_pos_pct = stop_pos;
        gradient->has_stop1_pos_pct = has_pos;

        if (gradient->stop_count < LXB_CSS_GRADIENT_MAX_STOPS) {
            const unsigned int i = gradient->stop_count;
            gradient->stops[i].color       = stop_color;
            gradient->stops[i].pos_pct     = stop_pos;
            gradient->stops[i].has_pos_pct = has_pos;
            gradient->stop_count = i + 1;
        }
        else {
            /* Overflow: keep the LAST stop in the final slot so the
             * gradient still ends on the author's final color. */
            const unsigned int last = LXB_CSS_GRADIENT_MAX_STOPS - 1;
            gradient->stops[last].color       = stop_color;
            gradient->stops[last].pos_pct     = stop_pos;
            gradient->stops[last].has_pos_pct = has_pos;
        }

        token = lxb_css_syntax_parser_token_wo_ws(parser);
    }

    if (token == NULL || token->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        return false;
    }
    lxb_css_syntax_parser_consume(parser);

    return true;
}

/*
 * String literals for gradient function names (lowercase, as the
 * tokenizer normalizes CSS function names to lowercase).
 */
static const lxb_char_t lxb_css_str_linear_gradient[] = "linear-gradient";
static const lxb_char_t lxb_css_str_repeating_linear_gradient[] =
    "repeating-linear-gradient";
static const lxb_char_t lxb_css_str_radial_gradient[] = "radial-gradient";
static const lxb_char_t lxb_css_str_var[] = "var";

static bool
lxb_css_property_state_is_function_named(const lxb_css_syntax_token_t *token,
                                         const lxb_char_t *name, size_t length)
{
    if (token == NULL || token->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION) {
        return false;
    }

    return lxb_css_syntax_token_function(token)->length == length
        && lexbor_str_data_ncasecmp(lxb_css_syntax_token_function(token)->data,
                                    name, length);
}

bool
lxb_css_property_state_background(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_status_t status;
    lxb_css_value_type_t type;
    lxb_css_value_color_t color;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);

        if (lxb_css_property_state_is_global(type)) {
            declar->u.background->color.type = type;

            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }
    }

    do {
        /* Check for gradient function tokens first. */
        if (token->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION) {
            const lxb_char_t *fname = lxb_css_syntax_token_function(token)->data;
            size_t            flen  = lxb_css_syntax_token_function(token)->length;
            lxb_css_gradient_kind_t gkind = LXB_CSS_GRADIENT_NONE;
            bool repeating_linear = false;

            if (lxb_css_property_state_is_function_named(token, lxb_css_str_var,
                    sizeof(lxb_css_str_var) - 1))
            {
                return lxb_css_parser_failed(parser);
            }

            if (flen == sizeof(lxb_css_str_linear_gradient) - 1
                && lexbor_str_data_ncasecmp(fname,
                       lxb_css_str_linear_gradient,
                       sizeof(lxb_css_str_linear_gradient) - 1))
            {
                gkind = LXB_CSS_GRADIENT_LINEAR;
            }
            else if (flen == sizeof(lxb_css_str_repeating_linear_gradient) - 1
                     && lexbor_str_data_ncasecmp(fname,
                            lxb_css_str_repeating_linear_gradient,
                            sizeof(lxb_css_str_repeating_linear_gradient) - 1))
            {
                gkind = LXB_CSS_GRADIENT_LINEAR;
                repeating_linear = true;
            }
            else if (flen == sizeof(lxb_css_str_radial_gradient) - 1
                     && lexbor_str_data_ncasecmp(fname,
                            lxb_css_str_radial_gradient,
                            sizeof(lxb_css_str_radial_gradient) - 1))
            {
                gkind = LXB_CSS_GRADIENT_RADIAL;
            }

            if (gkind != LXB_CSS_GRADIENT_NONE) {
                lxb_css_property_gradient_t gradient;

                /* Consume the function token (the `(` is part of it). */
                lxb_css_syntax_parser_consume(parser);

                if (lxb_css_property_state_gradient_args(parser, gkind,
                        &gradient))
                {
                    if (repeating_linear) {
                        gradient.kind = LXB_CSS_GRADIENT_LINEAR_STRIPES;
                    }
                    declar->u.background->gradient = gradient;
                    if (declar->u.background->layer_count < 3) {
                        declar->u.background->layers[
                            declar->u.background->layer_count++] = gradient;
                    }
                    /* Successfully parsed — no solid color from this token. */
                }
                else {
                    return lxb_css_parser_failed(parser);
                }

                token = lxb_css_syntax_parser_token_wo_ws(parser);
                lxb_css_property_state_check_token(parser, token);
                continue;
            }
        }

        color.type = LXB_CSS_VALUE__UNDEF;

        if (lxb_css_property_state_color_handler(parser, token, &color, &status)) {
            declar->u.background->color = color;
        }
        else {
            if (status != LXB_STATUS_OK) {
                return lxb_css_parser_failed(parser);
            }

            lxb_css_syntax_parser_consume(parser);
        }

        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }
    while (token->type != LXB_CSS_SYNTAX_TOKEN__END);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_background_color(lxb_css_parser_t *parser,
                                        const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_color(parser, token, ctx);
}

static bool
lxb_css_property_state_background_size_component(lxb_css_parser_t *parser,
                                                 const lxb_css_syntax_token_t *token,
                                                 lxb_css_value_length_percentage_t *out,
                                                 bool allow_global)
{
    lxb_css_value_type_t type;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);

        if (type == LXB_CSS_VALUE_AUTO
            || (allow_global && lxb_css_property_state_is_global(type)))
        {
            out->type = type;
            lxb_css_syntax_parser_consume(parser);
            return true;
        }

        return false;
    }

    return lxb_css_property_state_length_percentage(parser, token, out);
}

bool
lxb_css_property_state_background_size(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_background_size_layer_t layer;

    declar->u.background_size->layer_count = 0;

    while (token->type != LXB_CSS_SYNTAX_TOKEN__END) {
        layer.width.type = LXB_CSS_VALUE_AUTO;
        layer.height.type = LXB_CSS_VALUE_AUTO;

        if (!lxb_css_property_state_background_size_component(
                parser, token, &layer.width, true))
        {
            return lxb_css_parser_failed(parser);
        }

        if (lxb_css_property_state_is_global(layer.width.type)) {
            layer.height.type = layer.width.type;
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);
            if (token->type != LXB_CSS_SYNTAX_TOKEN__END) {
                return lxb_css_parser_failed(parser);
            }
        }
        else {
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);

            if (token->type != LXB_CSS_SYNTAX_TOKEN__END
                && token->type != LXB_CSS_SYNTAX_TOKEN_COMMA)
            {
                if (!lxb_css_property_state_background_size_component(
                        parser, token, &layer.height, false))
                {
                    return lxb_css_parser_failed(parser);
                }

                token = lxb_css_syntax_parser_token_wo_ws(parser);
                lxb_css_property_state_check_token(parser, token);
            }
        }

        if (declar->u.background_size->layer_count < 3) {
            declar->u.background_size->layers[
                declar->u.background_size->layer_count++] = layer;
        }

        if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
            return lxb_css_parser_success(parser);
        }

        if (token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
            return lxb_css_parser_failed(parser);
        }

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_color(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_status_t status;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            /* Global. */
            case LXB_CSS_VALUE_INITIAL:
            case LXB_CSS_VALUE_INHERIT:
            case LXB_CSS_VALUE_UNSET:
            case LXB_CSS_VALUE_REVERT:
                declar->u.color->type = type;

                lxb_css_syntax_parser_consume(parser);
                return lxb_css_parser_success(parser);

            default:
                break;
        }
    }

    res = lxb_css_property_state_color_handler(parser, token,
                                    (lxb_css_value_color_t *) declar->u.color,
                                    &status);
    if (!res) {
        return lxb_css_parser_failed(parser);
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_content(lxb_css_parser_t *parser,
                               const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_content_t *content = declar->u.content;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_STRING) {
        content->type = LXB_CSS_CONTENT_STRING;
        lxb_css_parser_string_dup_m(parser, token, &content->value,
                                    parser->memory->mraw);

        lxb_css_syntax_parser_consume(parser);
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        case LXB_CSS_VALUE_NORMAL:
        case LXB_CSS_VALUE_NONE:
            content->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_opacity(lxb_css_parser_t *parser,
                               const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_opacity_t *opacity = declar->u.opacity;

    res = lxb_css_property_state_number_percentage(parser, token,
                                (lxb_css_value_number_percentage_t *) opacity);
    if (res) {
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            opacity->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

static bool
lxb_css_transform_function_is(const lxb_css_syntax_token_t *token,
                              const char *name, size_t length)
{
    return token->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION
        && lxb_css_syntax_token_function(token)->length == length
        && lexbor_str_data_ncasecmp(lxb_css_syntax_token_function(token)->data,
                                    (const lxb_char_t *) name, length);
}

static void
lxb_css_transform_set_zero_length(lxb_css_value_length_percentage_t *value)
{
    value->type = LXB_CSS_VALUE__NUMBER;
    value->u.length.num = 0;
    value->u.length.is_float = false;
    value->u.length.unit = LXB_CSS_UNIT__UNDEF;
}

static bool
lxb_css_transform_consume_comma(lxb_css_parser_t *parser,
                                const lxb_css_syntax_token_t **token)
{
    *token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, *token);

    if ((*token)->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);
    return true;
}

static bool
lxb_css_transform_expect_rparen(lxb_css_parser_t *parser)
{
    const lxb_css_syntax_token_t *token;

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        return false;
    }

    lxb_css_syntax_parser_consume(parser);
    return true;
}

static bool
lxb_css_transform_parse_translate(lxb_css_parser_t *parser,
                                  lxb_css_transform_function_t *fn,
                                  lxb_css_transform_function_type_t type)
{
    bool res;
    const lxb_css_syntax_token_t *token;

    fn->type = type;
    lxb_css_transform_set_zero_length(&fn->x);
    lxb_css_transform_set_zero_length(&fn->y);

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (type == LXB_CSS_TRANSFORM_FUNCTION_TRANSLATE_Y) {
        res = lxb_css_property_state_length_percentage(parser, token, &fn->y);
        return res && lxb_css_transform_expect_rparen(parser);
    }

    res = lxb_css_property_state_length_percentage(parser, token, &fn->x);
    if (!res) return false;

    if (type == LXB_CSS_TRANSFORM_FUNCTION_TRANSLATE_X) {
        return lxb_css_transform_expect_rparen(parser);
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);
    if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        lxb_css_syntax_parser_consume(parser);
        return true;
    }

    (void) lxb_css_transform_consume_comma(parser, &token);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_length_percentage(parser, token, &fn->y);
    return res && lxb_css_transform_expect_rparen(parser);
}

static bool
lxb_css_transform_parse_scale(lxb_css_parser_t *parser,
                              lxb_css_transform_function_t *fn,
                              lxb_css_transform_function_type_t type)
{
    bool res;
    const lxb_css_syntax_token_t *token;

    fn->type = type;
    fn->numbers[0].num = 1;
    fn->numbers[0].is_float = false;
    fn->numbers[1].num = 1;
    fn->numbers[1].is_float = false;

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (type == LXB_CSS_TRANSFORM_FUNCTION_SCALE_Y) {
        res = lxb_css_property_state_number(parser, token, &fn->numbers[1]);
        return res && lxb_css_transform_expect_rparen(parser);
    }

    res = lxb_css_property_state_number(parser, token, &fn->numbers[0]);
    if (!res) return false;

    if (type == LXB_CSS_TRANSFORM_FUNCTION_SCALE_X) {
        return lxb_css_transform_expect_rparen(parser);
    }

    fn->numbers[1] = fn->numbers[0];

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);
    if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        lxb_css_syntax_parser_consume(parser);
        return true;
    }

    (void) lxb_css_transform_consume_comma(parser, &token);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number(parser, token, &fn->numbers[1]);
    return res && lxb_css_transform_expect_rparen(parser);
}

static bool
lxb_css_transform_parse_rotate(lxb_css_parser_t *parser,
                               lxb_css_transform_function_t *fn)
{
    bool res;
    const lxb_css_syntax_token_t *token;

    fn->type = LXB_CSS_TRANSFORM_FUNCTION_ROTATE;

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_angle(parser, token, &fn->angle);
    return res && lxb_css_transform_expect_rparen(parser);
}

static bool
lxb_css_transform_parse_matrix(lxb_css_parser_t *parser,
                               lxb_css_transform_function_t *fn)
{
    bool res;
    size_t i;
    const lxb_css_syntax_token_t *token;

    fn->type = LXB_CSS_TRANSFORM_FUNCTION_MATRIX;

    lxb_css_syntax_parser_consume(parser);
    for (i = 0; i < 6; i++) {
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        res = lxb_css_property_state_number(parser, token, &fn->numbers[i]);
        if (!res) return false;

        if (i + 1 < 6) {
            (void) lxb_css_transform_consume_comma(parser, &token);
        }
    }

    return lxb_css_transform_expect_rparen(parser);
}

bool
lxb_css_property_state_transform(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token,
                                 void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_transform_t *transform = declar->u.transform;

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_VALUE_INITIAL:
            case LXB_CSS_VALUE_INHERIT:
            case LXB_CSS_VALUE_UNSET:
            case LXB_CSS_VALUE_REVERT:
            case LXB_CSS_VALUE_NONE:
                transform->type = LXB_CSS_TRANSFORM_VALUE_NONE;
                transform->count = 0;
                lxb_css_syntax_parser_consume(parser);
                return lxb_css_parser_success(parser);

            default:
                return lxb_css_parser_failed(parser);
        }
    }

    transform->type = LXB_CSS_TRANSFORM_VALUE_LIST;
    transform->count = 0;

    while (token->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION) {
        lxb_css_transform_function_t *fn;

        if (transform->count >= 8) {
            return lxb_css_parser_failed(parser);
        }

        fn = &transform->functions[transform->count];

        if (lxb_css_transform_function_is(token, "translate", 9)) {
            if (!lxb_css_transform_parse_translate(parser, fn,
                    LXB_CSS_TRANSFORM_FUNCTION_TRANSLATE)) {
                return lxb_css_parser_failed(parser);
            }
        }
        else if (lxb_css_transform_function_is(token, "translatex", 10)) {
            if (!lxb_css_transform_parse_translate(parser, fn,
                    LXB_CSS_TRANSFORM_FUNCTION_TRANSLATE_X)) {
                return lxb_css_parser_failed(parser);
            }
        }
        else if (lxb_css_transform_function_is(token, "translatey", 10)) {
            if (!lxb_css_transform_parse_translate(parser, fn,
                    LXB_CSS_TRANSFORM_FUNCTION_TRANSLATE_Y)) {
                return lxb_css_parser_failed(parser);
            }
        }
        else if (lxb_css_transform_function_is(token, "scale", 5)) {
            if (!lxb_css_transform_parse_scale(parser, fn,
                    LXB_CSS_TRANSFORM_FUNCTION_SCALE)) {
                return lxb_css_parser_failed(parser);
            }
        }
        else if (lxb_css_transform_function_is(token, "scalex", 6)) {
            if (!lxb_css_transform_parse_scale(parser, fn,
                    LXB_CSS_TRANSFORM_FUNCTION_SCALE_X)) {
                return lxb_css_parser_failed(parser);
            }
        }
        else if (lxb_css_transform_function_is(token, "scaley", 6)) {
            if (!lxb_css_transform_parse_scale(parser, fn,
                    LXB_CSS_TRANSFORM_FUNCTION_SCALE_Y)) {
                return lxb_css_parser_failed(parser);
            }
        }
        else if (lxb_css_transform_function_is(token, "rotate", 6)) {
            if (!lxb_css_transform_parse_rotate(parser, fn)) {
                return lxb_css_parser_failed(parser);
            }
        }
        else if (lxb_css_transform_function_is(token, "rotatex", 7)) {
            if (!lxb_css_transform_parse_rotate(parser, fn)) {
                return lxb_css_parser_failed(parser);
            }
            fn->type = LXB_CSS_TRANSFORM_FUNCTION_ROTATE_X;
        }
        else if (lxb_css_transform_function_is(token, "rotatey", 7)) {
            if (!lxb_css_transform_parse_rotate(parser, fn)) {
                return lxb_css_parser_failed(parser);
            }
            fn->type = LXB_CSS_TRANSFORM_FUNCTION_ROTATE_Y;
        }
        else if (lxb_css_transform_function_is(token, "rotatez", 7)) {
            if (!lxb_css_transform_parse_rotate(parser, fn)) {
                return lxb_css_parser_failed(parser);
            }
            fn->type = LXB_CSS_TRANSFORM_FUNCTION_ROTATE_Z;
        }
        else if (lxb_css_transform_function_is(token, "matrix", 6)) {
            if (!lxb_css_transform_parse_matrix(parser, fn)) {
                return lxb_css_parser_failed(parser);
            }
        }
        else {
            return lxb_css_parser_failed(parser);
        }

        transform->count++;
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }

    if (transform->count == 0) {
        return lxb_css_parser_failed(parser);
    }

    return lxb_css_parser_success(parser);
}

static void
lxb_css_transform_origin_set_pct(lxb_css_value_length_percentage_t *out,
                                 double pct)
{
    out->type = LXB_CSS_VALUE__PERCENTAGE;
    out->u.percentage.num = pct;
    out->u.percentage.is_float = false;
}

static bool
lxb_css_transform_origin_keyword(const lxb_css_syntax_token_t *token,
                                 lxb_css_value_type_t *type)
{
    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return false;
    }

    *type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                  lxb_css_syntax_token_ident(token)->length);
    switch (*type) {
        case LXB_CSS_VALUE_LEFT:
        case LXB_CSS_VALUE_RIGHT:
        case LXB_CSS_VALUE_TOP:
        case LXB_CSS_VALUE_BOTTOM:
        case LXB_CSS_VALUE_CENTER:
            return true;
        default:
            return false;
    }
}

static bool
lxb_css_transform_origin_keyword_to_axis(lxb_css_value_type_t type,
                                         bool *is_x,
                                         lxb_css_value_length_percentage_t *out)
{
    switch (type) {
        case LXB_CSS_VALUE_LEFT:
            *is_x = true;
            lxb_css_transform_origin_set_pct(out, 0);
            return true;
        case LXB_CSS_VALUE_RIGHT:
            *is_x = true;
            lxb_css_transform_origin_set_pct(out, 100);
            return true;
        case LXB_CSS_VALUE_TOP:
            *is_x = false;
            lxb_css_transform_origin_set_pct(out, 0);
            return true;
        case LXB_CSS_VALUE_BOTTOM:
            *is_x = false;
            lxb_css_transform_origin_set_pct(out, 100);
            return true;
        case LXB_CSS_VALUE_CENTER:
            lxb_css_transform_origin_set_pct(out, 50);
            return true;
        default:
            return false;
    }
}

bool
lxb_css_property_state_transform_origin(lxb_css_parser_t *parser,
                                        const lxb_css_syntax_token_t *token,
                                        void *ctx)
{
    bool have_x, have_y, kw_is_x;
    lxb_css_value_type_t type;
    lxb_css_value_length_percentage_t first, second, kw_value;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_transform_origin_t *origin = declar->u.transform_origin;

    lxb_css_transform_origin_set_pct(&origin->x, 50);
    lxb_css_transform_origin_set_pct(&origin->y, 50);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_VALUE_INITIAL:
            case LXB_CSS_VALUE_INHERIT:
            case LXB_CSS_VALUE_UNSET:
            case LXB_CSS_VALUE_REVERT:
                lxb_css_syntax_parser_consume(parser);
                return lxb_css_parser_success(parser);
            default:
                break;
        }
    }

    have_x = false;
    have_y = false;

    if (lxb_css_property_state_length_percentage(parser, token, &first)) {
        origin->x = first;
        have_x = true;
    }
    else if (lxb_css_transform_origin_keyword(token, &type)) {
        if (!lxb_css_transform_origin_keyword_to_axis(type, &kw_is_x,
                                                      &kw_value)) {
            return lxb_css_parser_failed(parser);
        }

        if (type == LXB_CSS_VALUE_CENTER) {
            origin->x = kw_value;
            origin->y = kw_value;
            have_x = true;
            have_y = true;
        }
        else if (kw_is_x) {
            origin->x = kw_value;
            have_x = true;
        }
        else {
            origin->y = kw_value;
            have_y = true;
        }

        lxb_css_syntax_parser_consume(parser);
    }
    else {
        return lxb_css_parser_failed(parser);
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);
    if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
        return lxb_css_parser_success(parser);
    }

    if (lxb_css_property_state_length_percentage(parser, token, &second)) {
        if (have_y) {
            return lxb_css_parser_failed(parser);
        }
        origin->y = second;
        have_y = true;
    }
    else if (lxb_css_transform_origin_keyword(token, &type)) {
        if (!lxb_css_transform_origin_keyword_to_axis(type, &kw_is_x,
                                                      &kw_value)) {
            return lxb_css_parser_failed(parser);
        }

        if (type == LXB_CSS_VALUE_CENTER) {
            if (!have_x) {
                origin->x = kw_value;
                have_x = true;
            }
            else if (!have_y) {
                origin->y = kw_value;
                have_y = true;
            }
            else {
                return lxb_css_parser_failed(parser);
            }
        }
        else if (kw_is_x) {
            if (have_x) return lxb_css_parser_failed(parser);
            origin->x = kw_value;
            have_x = true;
        }
        else {
            if (have_y) return lxb_css_parser_failed(parser);
            origin->y = kw_value;
            have_y = true;
        }

        lxb_css_syntax_parser_consume(parser);
    }
    else {
        return lxb_css_parser_failed(parser);
    }

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);
    if (token->type != LXB_CSS_SYNTAX_TOKEN__END) {
        return lxb_css_parser_failed(parser);
    }

    (void) have_x;
    (void) have_y;
    return lxb_css_parser_success(parser);
}

static bool
lxb_css_animation_ident_is(const lxb_css_syntax_token_t *token,
                           const char *name, size_t length)
{
    return token->type == LXB_CSS_SYNTAX_TOKEN_IDENT
        && lxb_css_syntax_token_ident(token)->length == length
        && lexbor_str_data_ncasecmp(lxb_css_syntax_token_ident(token)->data,
                                    (const lxb_char_t *) name, length);
}

static bool
lxb_css_animation_parse_time(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token,
                             double *seconds)
{
    if (token->type == LXB_CSS_SYNTAX_TOKEN_DIMENSION) {
        const lxb_css_syntax_token_dimension_t *dim;

        dim = lxb_css_syntax_token_dimension(token);
        if (dim->str.length == 1 &&
            lexbor_str_data_ncasecmp(dim->str.data, (const lxb_char_t *) "s", 1)) {
            *seconds = dim->num.num;
        }
        else if (dim->str.length == 2 &&
                 lexbor_str_data_ncasecmp(dim->str.data, (const lxb_char_t *) "ms", 2)) {
            *seconds = dim->num.num / 1000.0;
        }
        else {
            return false;
        }

        lxb_css_syntax_parser_consume(parser);
        return true;
    }

    if (token->type == LXB_CSS_SYNTAX_TOKEN_NUMBER &&
        lxb_css_syntax_token_number(token)->num == 0.0) {
        *seconds = 0.0;
        lxb_css_syntax_parser_consume(parser);
        return true;
    }

    return false;
}

static bool
lxb_css_animation_parse_timing(const lxb_css_syntax_token_t *token,
                               lxb_css_animation_timing_function_type_t *timing)
{
    if (lxb_css_animation_ident_is(token, "ease", 4)) {
        *timing = LXB_CSS_ANIMATION_TIMING_EASE;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "linear", 6)) {
        *timing = LXB_CSS_ANIMATION_TIMING_LINEAR;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "ease-in", 7)) {
        *timing = LXB_CSS_ANIMATION_TIMING_EASE_IN;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "ease-out", 8)) {
        *timing = LXB_CSS_ANIMATION_TIMING_EASE_OUT;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "ease-in-out", 11)) {
        *timing = LXB_CSS_ANIMATION_TIMING_EASE_IN_OUT;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "step-start", 10)) {
        *timing = LXB_CSS_ANIMATION_TIMING_STEP_START;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "step-end", 8)) {
        *timing = LXB_CSS_ANIMATION_TIMING_STEP_END;
        return true;
    }
    return false;
}

static bool
lxb_css_animation_parse_direction(const lxb_css_syntax_token_t *token,
                                  lxb_css_animation_direction_type_t *direction)
{
    if (lxb_css_animation_ident_is(token, "normal", 6)) {
        *direction = LXB_CSS_ANIMATION_DIRECTION_NORMAL;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "reverse", 7)) {
        *direction = LXB_CSS_ANIMATION_DIRECTION_REVERSE;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "alternate", 9)) {
        *direction = LXB_CSS_ANIMATION_DIRECTION_ALTERNATE;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "alternate-reverse", 17)) {
        *direction = LXB_CSS_ANIMATION_DIRECTION_ALTERNATE_REVERSE;
        return true;
    }
    return false;
}

static bool
lxb_css_animation_parse_fill_mode(const lxb_css_syntax_token_t *token,
                                  lxb_css_animation_fill_mode_type_t *fill)
{
    if (lxb_css_animation_ident_is(token, "none", 4)) {
        *fill = LXB_CSS_ANIMATION_FILL_MODE_NONE;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "forwards", 8)) {
        *fill = LXB_CSS_ANIMATION_FILL_MODE_FORWARDS;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "backwards", 9)) {
        *fill = LXB_CSS_ANIMATION_FILL_MODE_BACKWARDS;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "both", 4)) {
        *fill = LXB_CSS_ANIMATION_FILL_MODE_BOTH;
        return true;
    }
    return false;
}

static bool
lxb_css_animation_parse_play_state(const lxb_css_syntax_token_t *token,
                                   lxb_css_animation_play_state_type_t *state)
{
    if (lxb_css_animation_ident_is(token, "running", 7)) {
        *state = LXB_CSS_ANIMATION_PLAY_STATE_RUNNING;
        return true;
    }
    if (lxb_css_animation_ident_is(token, "paused", 6)) {
        *state = LXB_CSS_ANIMATION_PLAY_STATE_PAUSED;
        return true;
    }
    return false;
}

static bool
lxb_css_animation_parse_name(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token,
                             lxb_css_property_animation_t *animation)
{
    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        if (lxb_css_animation_ident_is(token, "none", 4)) {
            animation->has_name = false;
            lxb_css_syntax_parser_consume(parser);
            return true;
        }
        lxb_css_parser_string_dup_m(parser, token, &animation->name,
                                    parser->memory->mraw);
        animation->has_name = true;
        lxb_css_syntax_parser_consume(parser);
        return true;
    }

    if (token->type == LXB_CSS_SYNTAX_TOKEN_STRING) {
        lxb_css_parser_string_dup_m(parser, token, &animation->name,
                                    parser->memory->mraw);
        animation->has_name = true;
        lxb_css_syntax_parser_consume(parser);
        return true;
    }

    return false;
}

bool
lxb_css_property_state_animation(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token,
                                 void *ctx)
{
    bool consumed = false;
    bool duration_set = false;
    bool delay_set = false;
    bool timing_set = false;
    bool iteration_set = false;
    bool direction_set = false;
    bool fill_set = false;
    bool play_set = false;
    bool name_set = false;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_animation_t *animation = declar->u.animation;

    while (token != NULL) {
        double seconds = 0.0;

        if (!duration_set &&
            lxb_css_animation_parse_time(parser, token, &seconds)) {
            animation->duration_s = seconds;
            duration_set = true;
            consumed = true;
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            continue;
        }
        if (duration_set && !delay_set &&
            lxb_css_animation_parse_time(parser, token, &seconds)) {
            animation->delay_s = seconds;
            delay_set = true;
            consumed = true;
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            continue;
        }

        if (token->type == LXB_CSS_SYNTAX_TOKEN_NUMBER && !iteration_set) {
            animation->iteration_count = lxb_css_syntax_token_number(token)->num;
            iteration_set = true;
            consumed = true;
            lxb_css_syntax_parser_consume(parser);
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            continue;
        }

        if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
            if (!timing_set &&
                lxb_css_animation_parse_timing(token, &animation->timing)) {
                timing_set = true;
                consumed = true;
                lxb_css_syntax_parser_consume(parser);
            }
            else if (!iteration_set &&
                     lxb_css_animation_ident_is(token, "infinite", 8)) {
                animation->iteration_count = 0.0;
                iteration_set = true;
                consumed = true;
                lxb_css_syntax_parser_consume(parser);
            }
            else if (!direction_set &&
                     lxb_css_animation_parse_direction(token, &animation->direction)) {
                direction_set = true;
                consumed = true;
                lxb_css_syntax_parser_consume(parser);
            }
            else if (!fill_set &&
                     lxb_css_animation_parse_fill_mode(token, &animation->fill_mode)) {
                fill_set = true;
                consumed = true;
                lxb_css_syntax_parser_consume(parser);
            }
            else if (!play_set &&
                     lxb_css_animation_parse_play_state(token, &animation->play_state)) {
                play_set = true;
                consumed = true;
                lxb_css_syntax_parser_consume(parser);
            }
            else if (!name_set &&
                     lxb_css_animation_parse_name(parser, token, animation)) {
                name_set = true;
                consumed = true;
            }
            else {
                break;
            }

            token = lxb_css_syntax_parser_token_wo_ws(parser);
            continue;
        }

        if (!name_set &&
            token->type == LXB_CSS_SYNTAX_TOKEN_STRING &&
            lxb_css_animation_parse_name(parser, token, animation)) {
            name_set = true;
            consumed = true;
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            continue;
        }

        break;
    }

    if (consumed && (token == NULL
            || token->type == LXB_CSS_SYNTAX_TOKEN__END))
    {
        return lxb_css_parser_success(parser);
    }

    return lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_animation_name(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token,
                                      void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;
    return lxb_css_animation_parse_name(parser, token, declar->u.animation_name)
        ? lxb_css_parser_success(parser)
        : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_animation_duration(lxb_css_parser_t *parser,
                                          const lxb_css_syntax_token_t *token,
                                          void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;
    return lxb_css_animation_parse_time(parser, token,
                                        &declar->u.animation_duration->duration_s)
        ? lxb_css_parser_success(parser)
        : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_animation_timing_function(lxb_css_parser_t *parser,
                                                const lxb_css_syntax_token_t *token,
                                                void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;
    if (!lxb_css_animation_parse_timing(
            token, &declar->u.animation_timing_function->timing)) {
        return lxb_css_parser_failed(parser);
    }
    lxb_css_syntax_parser_consume(parser);
    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_animation_delay(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token,
                                       void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;
    return lxb_css_animation_parse_time(parser, token,
                                        &declar->u.animation_delay->delay_s)
        ? lxb_css_parser_success(parser)
        : lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_animation_iteration_count(lxb_css_parser_t *parser,
                                                const lxb_css_syntax_token_t *token,
                                                void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;
    if (token->type == LXB_CSS_SYNTAX_TOKEN_NUMBER) {
        declar->u.animation_iteration_count->iteration_count =
            lxb_css_syntax_token_number(token)->num;
        lxb_css_syntax_parser_consume(parser);
        return lxb_css_parser_success(parser);
    }
    if (lxb_css_animation_ident_is(token, "infinite", 8)) {
        declar->u.animation_iteration_count->iteration_count = 0.0;
        lxb_css_syntax_parser_consume(parser);
        return lxb_css_parser_success(parser);
    }
    return lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_animation_direction(lxb_css_parser_t *parser,
                                           const lxb_css_syntax_token_t *token,
                                           void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;
    if (!lxb_css_animation_parse_direction(
            token, &declar->u.animation_direction->direction)) {
        return lxb_css_parser_failed(parser);
    }
    lxb_css_syntax_parser_consume(parser);
    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_animation_fill_mode(lxb_css_parser_t *parser,
                                           const lxb_css_syntax_token_t *token,
                                           void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;
    if (!lxb_css_animation_parse_fill_mode(
            token, &declar->u.animation_fill_mode->fill_mode)) {
        return lxb_css_parser_failed(parser);
    }
    lxb_css_syntax_parser_consume(parser);
    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_animation_play_state(lxb_css_parser_t *parser,
                                            const lxb_css_syntax_token_t *token,
                                            void *ctx)
{
    lxb_css_rule_declaration_t *declar = ctx;
    if (!lxb_css_animation_parse_play_state(
            token, &declar->u.animation_play_state->play_state)) {
        return lxb_css_parser_failed(parser);
    }
    lxb_css_syntax_parser_consume(parser);
    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_position(lxb_css_parser_t *parser,
                                const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_POSITION_STATIC:
        case LXB_CSS_POSITION_RELATIVE:
        case LXB_CSS_POSITION_ABSOLUTE:
        case LXB_CSS_POSITION_STICKY:
        case LXB_CSS_POSITION_FIXED:
            declar->u.position->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_inset(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp(parser, token, ctx, true);
}

bool
lxb_css_property_state_top(lxb_css_parser_t *parser,
                           const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_mp_top(parser, token, ctx, true);
}

bool
lxb_css_property_state_right(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_top(parser, token, ctx);
}

bool
lxb_css_property_state_bottom(lxb_css_parser_t *parser,
                              const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_top(parser, token, ctx);
}

bool
lxb_css_property_state_left(lxb_css_parser_t *parser,
                            const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_top(parser, token, ctx);
}

bool
lxb_css_property_state_inset_block_start(lxb_css_parser_t *parser,
                                         const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_top(parser, token, ctx);
}

bool
lxb_css_property_state_inset_inline_start(lxb_css_parser_t *parser,
                                          const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_top(parser, token, ctx);
}

bool
lxb_css_property_state_inset_block_end(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_top(parser, token, ctx);
}

bool
lxb_css_property_state_inset_inline_end(lxb_css_parser_t *parser,
                                        const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_top(parser, token, ctx);
}

bool
lxb_css_property_state_text_transform(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_text_transform_t *tt;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    tt = declar->u.text_transform;

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_TEXT_TRANSFORM_NONE:
            tt->type_case = type;
            break;

        case LXB_CSS_TEXT_TRANSFORM_CAPITALIZE:
        case LXB_CSS_TEXT_TRANSFORM_UPPERCASE:
        case LXB_CSS_TEXT_TRANSFORM_LOWERCASE:
            tt->type_case = type;
            goto next;

        case LXB_CSS_TEXT_TRANSFORM_FULL_WIDTH:
            tt->full_width = type;
            goto next;

        case LXB_CSS_TEXT_TRANSFORM_FULL_SIZE_KANA:
            tt->full_size_kana = type;
            goto next;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);

next:

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_success(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);

    switch (type) {
        case LXB_CSS_TEXT_TRANSFORM_CAPITALIZE:
        case LXB_CSS_TEXT_TRANSFORM_UPPERCASE:
        case LXB_CSS_TEXT_TRANSFORM_LOWERCASE:
            if (tt->type_case != LXB_CSS_VALUE__UNDEF) {
                return lxb_css_parser_failed(parser);
            }

            tt->type_case = type;
            goto next;

        case LXB_CSS_TEXT_TRANSFORM_FULL_WIDTH:
            if (tt->full_width != LXB_CSS_VALUE__UNDEF) {
                return lxb_css_parser_failed(parser);
            }

            tt->full_width = type;
            goto next;

        case LXB_CSS_TEXT_TRANSFORM_FULL_SIZE_KANA:
            if (tt->full_size_kana != LXB_CSS_VALUE__UNDEF) {
                return lxb_css_parser_failed(parser);
            }

            tt->full_size_kana = type;
            goto next;

        default:
            return lxb_css_parser_failed(parser);
    }
}

bool
lxb_css_property_state_text_align(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_TEXT_ALIGN_START:
        case LXB_CSS_TEXT_ALIGN_END:
        case LXB_CSS_TEXT_ALIGN_LEFT:
        case LXB_CSS_TEXT_ALIGN_RIGHT:
        case LXB_CSS_TEXT_ALIGN_CENTER:
        case LXB_CSS_TEXT_ALIGN_JUSTIFY:
        case LXB_CSS_TEXT_ALIGN_MATCH_PARENT:
        case LXB_CSS_TEXT_ALIGN_JUSTIFY_ALL:
            declar->u.text_align->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_text_align_all(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_TEXT_ALIGN_ALL_START:
        case LXB_CSS_TEXT_ALIGN_ALL_END:
        case LXB_CSS_TEXT_ALIGN_ALL_LEFT:
        case LXB_CSS_TEXT_ALIGN_ALL_RIGHT:
        case LXB_CSS_TEXT_ALIGN_ALL_CENTER:
        case LXB_CSS_TEXT_ALIGN_ALL_JUSTIFY:
        case LXB_CSS_TEXT_ALIGN_ALL_MATCH_PARENT:
            declar->u.text_align_all->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_text_align_last(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_TEXT_ALIGN_LAST_AUTO:
        case LXB_CSS_TEXT_ALIGN_LAST_START:
        case LXB_CSS_TEXT_ALIGN_LAST_END:
        case LXB_CSS_TEXT_ALIGN_LAST_LEFT:
        case LXB_CSS_TEXT_ALIGN_LAST_RIGHT:
        case LXB_CSS_TEXT_ALIGN_LAST_CENTER:
        case LXB_CSS_TEXT_ALIGN_LAST_JUSTIFY:
        case LXB_CSS_TEXT_ALIGN_LAST_MATCH_PARENT:
            declar->u.text_align_last->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_text_justify(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_TEXT_JUSTIFY_AUTO:
        case LXB_CSS_TEXT_JUSTIFY_NONE:
        case LXB_CSS_TEXT_JUSTIFY_INTER_WORD:
        case LXB_CSS_TEXT_JUSTIFY_INTER_CHARACTER:
            declar->u.text_justify->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_text_indent(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_text_indent_t *text_indent;

    text_indent = declar->u.text_indent;

    res = lxb_css_property_state_length_percentage(parser, token,
                                                   &text_indent->length);
    if (res) {
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        text_indent->type = LXB_CSS_VALUE__LENGTH;
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        if (!res) {
            return lxb_css_parser_failed(parser);
        }

        return lxb_css_parser_success(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            if (res) {
                return lxb_css_parser_failed(parser);
            }

            text_indent->type = type;
            break;

        /* Local. */
        case LXB_CSS_TEXT_INDENT_HANGING:
            text_indent->hanging = type;
            goto next;

        case LXB_CSS_TEXT_INDENT_EACH_LINE:
            text_indent->each_line = type;
            goto next;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);

next:

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_length_percentage(parser, token,
                                                   &text_indent->length);
    if (res) {
        if (text_indent->type != LXB_CSS_VALUE__UNDEF) {
            return lxb_css_parser_failed(parser);
        }

        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        text_indent->type = LXB_CSS_VALUE__LENGTH;
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        if (text_indent->type == LXB_CSS_VALUE__UNDEF) {
            return lxb_css_parser_failed(parser);
        }

        return lxb_css_parser_success(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);

    switch (type) {
        case LXB_CSS_TEXT_INDENT_HANGING:
            if (text_indent->hanging != LXB_CSS_VALUE__UNDEF) {
                return lxb_css_parser_failed(parser);
            }

            text_indent->hanging = type;
            goto next;

        case LXB_CSS_TEXT_INDENT_EACH_LINE:
            if (text_indent->each_line != LXB_CSS_VALUE__UNDEF) {
                return lxb_css_parser_failed(parser);
            }

            text_indent->each_line = type;
            goto next;

        default:
            return lxb_css_parser_failed(parser);
    }
}

bool
lxb_css_property_state_white_space(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_WHITE_SPACE_NORMAL:
        case LXB_CSS_WHITE_SPACE_PRE:
        case LXB_CSS_WHITE_SPACE_NOWRAP:
        case LXB_CSS_WHITE_SPACE_PRE_WRAP:
        case LXB_CSS_WHITE_SPACE_BREAK_SPACES:
        case LXB_CSS_WHITE_SPACE_PRE_LINE:
            declar->u.white_space->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_tab_size(lxb_css_parser_t *parser,
                                const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    res = lxb_css_property_state_number_length(parser, token,
                         (lxb_css_value_number_length_t *) declar->u.tab_size);
    if (res) {
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            declar->u.tab_size->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_word_break(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_WORD_BREAK_NORMAL:
        case LXB_CSS_WORD_BREAK_KEEP_ALL:
        case LXB_CSS_WORD_BREAK_BREAK_ALL:
        case LXB_CSS_WORD_BREAK_BREAK_WORD:
            declar->u.word_break->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_line_break(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_LINE_BREAK_AUTO:
        case LXB_CSS_LINE_BREAK_LOOSE:
        case LXB_CSS_LINE_BREAK_NORMAL:
        case LXB_CSS_LINE_BREAK_STRICT:
        case LXB_CSS_LINE_BREAK_ANYWHERE:
            declar->u.line_break->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_hyphens(lxb_css_parser_t *parser,
                               const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_HYPHENS_NONE:
        case LXB_CSS_HYPHENS_MANUAL:
        case LXB_CSS_HYPHENS_AUTO:
            declar->u.hyphens->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_overflow_wrap(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_OVERFLOW_WRAP_NORMAL:
        case LXB_CSS_OVERFLOW_WRAP_BREAK_WORD:
        case LXB_CSS_OVERFLOW_WRAP_ANYWHERE:
            declar->u.overflow_wrap->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_word_wrap(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_overflow_wrap(parser, token, ctx);
}

bool
lxb_css_property_state_word_spacing(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    res = lxb_css_property_state_length(parser, token,
                                        &declar->u.word_spacing->length);
    if (res) {
        declar->u.word_spacing->type = LXB_CSS_VALUE__LENGTH;

        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_WORD_SPACING_NORMAL:
            declar->u.word_spacing->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_letter_spacing(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_word_spacing(parser, token, ctx);
}

bool
lxb_css_property_state_hanging_punctuation(lxb_css_parser_t *parser,
                                           const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_hanging_punctuation_t *hp;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    hp = declar->u.hanging_punctuation;

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
            /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            /* Local. */
        case LXB_CSS_HANGING_PUNCTUATION_NONE:
            hp->type_first = type;
            break;

        case LXB_CSS_HANGING_PUNCTUATION_FIRST:
            hp->type_first = type;
            goto next;

        case LXB_CSS_HANGING_PUNCTUATION_FORCE_END:
        case LXB_CSS_HANGING_PUNCTUATION_ALLOW_END:
            hp->force_allow = type;
            goto next;

        case LXB_CSS_HANGING_PUNCTUATION_LAST:
            hp->last = type;
            goto next;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);

next:

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_success(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);

    switch (type) {
        case LXB_CSS_HANGING_PUNCTUATION_FIRST:
            if (hp->type_first != LXB_CSS_VALUE__UNDEF) {
                return lxb_css_parser_failed(parser);
            }

            hp->type_first = type;
            goto next;

        case LXB_CSS_HANGING_PUNCTUATION_FORCE_END:
        case LXB_CSS_HANGING_PUNCTUATION_ALLOW_END:
            if (hp->force_allow != LXB_CSS_VALUE__UNDEF) {
                return lxb_css_parser_failed(parser);
            }

            hp->force_allow = type;
            goto next;

        case LXB_CSS_HANGING_PUNCTUATION_LAST:
            if (hp->last != LXB_CSS_VALUE__UNDEF) {
                return lxb_css_parser_failed(parser);
            }

            hp->last = type;
            goto next;

        default:
            return lxb_css_parser_failed(parser);
    }
}

/*
 * font shorthand parser.
 *
 * Grammar (simplified CSS2/CSS3):
 *   font: [ <font-style> || <font-weight> ]? <font-size> [ / <line-height> ]?
 *         <font-family>
 *       | <system-font>
 *
 * system-font keywords (caption|icon|menu|message-box|small-caption|status-bar)
 * are accepted and set type to the keyword value; AffineUI treats them as no-op.
 */
bool
lxb_css_property_state_font(lxb_css_parser_t *parser,
                            const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_font_t *font = declar->u.font;
    lexbor_mraw_t *mraw = parser->memory->mraw;
    lxb_css_property_family_name_t *name;

    /* Initialise sub-fields to sane defaults. */
    font->style.type   = LXB_CSS_FONT_STYLE_NORMAL;
    font->weight.type  = LXB_CSS_FONT_WEIGHT_NORMAL;
    font->line_height.type = LXB_CSS_LINE_HEIGHT_NORMAL;
    font->family.first = NULL;
    font->family.last  = NULL;
    font->family.count = 0;

    /* --- Optional leading ident: style or weight keyword --- */
    if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);

        switch (type) {
            /* Global values apply to the shorthand as a whole. */
            case LXB_CSS_VALUE_INITIAL:
            case LXB_CSS_VALUE_INHERIT:
            case LXB_CSS_VALUE_UNSET:
            case LXB_CSS_VALUE_REVERT:
                font->type = type;
                lxb_css_syntax_parser_consume(parser);
                return lxb_css_parser_success(parser);

            /* font-style keyword before font-size. */
            case LXB_CSS_FONT_STYLE_ITALIC:
            case LXB_CSS_FONT_STYLE_OBLIQUE:
                font->style.type = type;
                lxb_css_syntax_parser_consume(parser);
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                lxb_css_property_state_check_token(parser, token);
                break;

            /* font-weight keyword before font-size. */
            case LXB_CSS_FONT_WEIGHT_BOLD:
            case LXB_CSS_FONT_WEIGHT_BOLDER:
            case LXB_CSS_FONT_WEIGHT_LIGHTER:
                font->weight.type = type;
                lxb_css_syntax_parser_consume(parser);
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                lxb_css_property_state_check_token(parser, token);
                break;

            /* font-size keyword — fall through to size parsing below. */
            case LXB_CSS_FONT_SIZE_XX_SMALL:
            case LXB_CSS_FONT_SIZE_X_SMALL:
            case LXB_CSS_FONT_SIZE_SMALL:
            case LXB_CSS_FONT_SIZE_MEDIUM:
            case LXB_CSS_FONT_SIZE_LARGE:
            case LXB_CSS_FONT_SIZE_X_LARGE:
            case LXB_CSS_FONT_SIZE_XX_LARGE:
            case LXB_CSS_FONT_SIZE_XXX_LARGE:
            case LXB_CSS_FONT_SIZE_LARGER:
            case LXB_CSS_FONT_SIZE_SMALLER:
            case LXB_CSS_FONT_SIZE_MATH:
            case LXB_CSS_VALUE_NORMAL:
                /* normal could be a weight placeholder; treat as start of size */
                break;

            default:
                /* Unknown ident — might be a font-size keyword we don't know
                   or a font-family name; try size parsing and let it fail. */
                break;
        }
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_NUMBER
             && lxb_css_syntax_token_number(token)->num >= 1
             && lxb_css_syntax_token_number(token)->num <= 1000)
    {
        /* A leading number is a font-weight only when it is a valid weight
           (1-1000). state_number() consumes the token on success, so re-fetch
           the current token before it is read again below.

           Crucially, a number OUTSIDE that range is the font-size itself, not a
           weight (e.g. the "font:0/0 a" icon-reset hack, where 0 is the size).
           We must NOT consume it here, or the size parse below would read a
           recycled token (use-after-free) and the size would be lost. Leaving
           it untouched lets length_percentage() pick it up (it accepts 0). */
        res = lxb_css_property_state_number(parser, token, &font->weight.number);
        if (res) {
            font->weight.type = LXB_CSS_FONT_WEIGHT__NUMBER;
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);
        }
    }

    /* --- Required: font-size --- */
    res = lxb_css_property_state_length_percentage(parser, token, &font->size.length);
    if (res) {
        if (font->size.length.u.length.num < 0) {
            return lxb_css_parser_failed(parser);
        }
        font->size.type = LXB_CSS_FONT_SIZE__LENGTH;
    }
    else if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
        type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                     lxb_css_syntax_token_ident(token)->length);
        switch (type) {
            case LXB_CSS_FONT_SIZE_XX_SMALL:
            case LXB_CSS_FONT_SIZE_X_SMALL:
            case LXB_CSS_FONT_SIZE_SMALL:
            case LXB_CSS_FONT_SIZE_MEDIUM:
            case LXB_CSS_FONT_SIZE_LARGE:
            case LXB_CSS_FONT_SIZE_X_LARGE:
            case LXB_CSS_FONT_SIZE_XX_LARGE:
            case LXB_CSS_FONT_SIZE_XXX_LARGE:
            case LXB_CSS_FONT_SIZE_LARGER:
            case LXB_CSS_FONT_SIZE_SMALLER:
            case LXB_CSS_FONT_SIZE_MATH:
                font->size.type = type;
                lxb_css_syntax_parser_consume(parser);
                break;
            default:
                return lxb_css_parser_failed(parser);
        }
    }
    else {
        return lxb_css_parser_failed(parser);
    }

    font->type = LXB_CSS_FONT__DETAIL;

    /* --- Optional: / <line-height> --- */
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_DELIM
        && lxb_css_syntax_token_delim_char(token) == '/')
    {
        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        res = lxb_css_property_state_number_length_percentage(parser, token,
                                                              &font->line_height);
        if (!res) {
            if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
                type = lxb_css_value_by_name(
                    lxb_css_syntax_token_ident(token)->data,
                    lxb_css_syntax_token_ident(token)->length);
                if (type == LXB_CSS_LINE_HEIGHT_NORMAL) {
                    font->line_height.type = LXB_CSS_LINE_HEIGHT_NORMAL;
                    lxb_css_syntax_parser_consume(parser);
                } else {
                    return lxb_css_parser_failed(parser);
                }
            } else {
                return lxb_css_parser_failed(parser);
            }
        }

        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);
    }

    /* --- Required: font-family (one or more names) --- */
    while (token->type != LXB_CSS_SYNTAX_TOKEN__END) {
        name = lexbor_mraw_alloc(mraw, sizeof(lxb_css_property_family_name_t));
        if (name == NULL) {
            return lxb_css_parser_memory_fail(parser);
        }

        if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
            const lxb_char_t *data = lxb_css_syntax_token_ident(token)->data;
            size_t length = lxb_css_syntax_token_ident(token)->length;
            lxb_css_value_type_t ftype = lxb_css_value_by_name(data, length);
            if (ftype != LXB_CSS_VALUE__UNDEF) {
                name->generic = true;
                name->u.type = ftype;
            } else {
                name->generic = false;
                (void) lexbor_str_init(&name->u.str, mraw, length);
                if (name->u.str.data == NULL) {
                    return lxb_css_parser_memory_fail(parser);
                }
                memcpy(name->u.str.data, data, length);
                name->u.str.data[length] = '\0';
                name->u.str.length = length;
            }
        }
        else if (token->type == LXB_CSS_SYNTAX_TOKEN_STRING) {
            const lxb_char_t *data = lxb_css_syntax_token_string(token)->data;
            size_t length = lxb_css_syntax_token_string(token)->length;
            name->generic = false;
            (void) lexbor_str_init(&name->u.str, mraw, length);
            if (name->u.str.data == NULL) {
                return lxb_css_parser_memory_fail(parser);
            }
            memcpy(name->u.str.data, data, length);
            name->u.str.data[length] = '\0';
            name->u.str.length = length;
        }
        else {
            /* Unexpected token — the family list is done. */
            break;
        }

        name->next = NULL;
        name->prev = font->family.last;

        if (font->family.first == NULL) {
            font->family.first = name;
        } else {
            font->family.last->next = name;
        }
        font->family.last = name;
        font->family.count++;

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        if (token->type == LXB_CSS_SYNTAX_TOKEN_COMMA) {
            lxb_css_syntax_parser_consume(parser);
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);
        }
    }

    if (font->family.first == NULL) {
        return lxb_css_parser_failed(parser);
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_font_family(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    size_t length;
    const lxb_char_t *data;
    lexbor_str_t *str;
    lexbor_mraw_t *mraw;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_font_family_t *ff;
    lxb_css_property_family_name_t *name;

    mraw = parser->memory->mraw;
    ff = declar->u.font_family;

    while (token != NULL) {
        name = lexbor_mraw_alloc(mraw, sizeof(lxb_css_property_family_name_t));
        if (name == NULL) {
            return lxb_css_parser_memory_fail(parser);
        }

        if (token->type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
            data = lxb_css_syntax_token_ident(token)->data;
            length = lxb_css_syntax_token_ident(token)->length;

            type = lxb_css_value_by_name(data, length);
            if (type != LXB_CSS_VALUE__UNDEF) {
                name->generic = true;
                name->u.type = type;

                goto next;
            }
        }
        else if (token->type == LXB_CSS_SYNTAX_TOKEN_STRING) {
            data = lxb_css_syntax_token_string(token)->data;
            length = lxb_css_syntax_token_string(token)->length;
        }
        else {
            return lxb_css_parser_failed(parser);
        }

        name->generic = false;

        str = &name->u.str;

        (void) lexbor_str_init(str, mraw, length);
        if (name->u.str.data == NULL) {
            return lxb_css_parser_memory_fail(parser);
        }

        memcpy(str->data, data, length);

        str->data[length] = '\0';
        str->length = length;

    next:

        if (ff->first == NULL) {
            ff->first = name;
        }
        else {
            ff->last->next = name;
        }

        name->next = NULL;
        name->prev = ff->last;
        ff->last = name;

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        if (token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
            if (token->type == LXB_CSS_SYNTAX_TOKEN__END) {
                return lxb_css_parser_success(parser);
            }

            return lxb_css_parser_memory_fail(parser);
        }

        lxb_css_syntax_parser_consume(parser);
        token = lxb_css_syntax_parser_token_wo_ws(parser);
    }

    lxb_css_property_state_check_token(parser, token);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_font_weight(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)\
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_font_weight_t *fw = declar->u.font_weight;

    res = lxb_css_property_state_number(parser, token, &fw->number);

    if (res) {
        if (fw->number.num < 1 || fw->number.num > 1000) {
            return lxb_css_parser_failed(parser);
        }

        fw->type = LXB_CSS_FONT_WEIGHT__NUMBER;
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_FONT_WEIGHT_NORMAL:
        case LXB_CSS_FONT_WEIGHT_BOLD:
        case LXB_CSS_FONT_WEIGHT_BOLDER:
        case LXB_CSS_FONT_WEIGHT_LIGHTER:
            fw->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_font_stretch(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_font_stretch_t *fs = declar->u.font_stretch;

    res = lxb_css_property_state_percentage(parser, token, &fs->percentage);

    if (res) {
        if (fs->percentage.num < 0) {
            return lxb_css_parser_failed(parser);
        }

        fs->type = LXB_CSS_FONT_STRETCH__PERCENTAGE;
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_FONT_STRETCH_NORMAL:
        case LXB_CSS_FONT_STRETCH_ULTRA_CONDENSED:
        case LXB_CSS_FONT_STRETCH_EXTRA_CONDENSED:
        case LXB_CSS_FONT_STRETCH_CONDENSED:
        case LXB_CSS_FONT_STRETCH_SEMI_CONDENSED:
        case LXB_CSS_FONT_STRETCH_SEMI_EXPANDED:
        case LXB_CSS_FONT_STRETCH_EXPANDED:
        case LXB_CSS_FONT_STRETCH_EXTRA_EXPANDED:
        case LXB_CSS_FONT_STRETCH_ULTRA_EXPANDED:
            fs->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_font_style(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_font_style_t *fs = declar->u.font_style;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
            /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            /* Local. */
        case LXB_CSS_FONT_STYLE_NORMAL:
        case LXB_CSS_FONT_STYLE_ITALIC:
            fs->type = type;
            break;

        case LXB_CSS_FONT_STYLE_OBLIQUE:
            fs->type = type;

            lxb_css_syntax_parser_consume(parser);
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);

            res = lxb_css_property_state_angle(parser, token, &fs->angle);

            if (res) {
                if (fs->angle.num < -90 || fs->angle.num > 90) {
                    return lxb_css_parser_failed(parser);
                }

                return lxb_css_parser_success(parser);
            }
            else {
                fs->angle.unit = (lxb_css_unit_angel_t) LXB_CSS_UNIT__UNDEF;
            }

            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_font_size(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_font_size_t *fs = declar->u.font_size;

    res = lxb_css_property_state_length_percentage(parser, token, &fs->length);

    if (res) {
        if (fs->length.u.length.num < 0) {
            return lxb_css_parser_failed(parser);
        }

        fs->type = LXB_CSS_FONT_SIZE__LENGTH;

        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_FONT_SIZE_XX_SMALL:
        case LXB_CSS_FONT_SIZE_X_SMALL:
        case LXB_CSS_FONT_SIZE_SMALL:
        case LXB_CSS_FONT_SIZE_MEDIUM:
        case LXB_CSS_FONT_SIZE_LARGE:
        case LXB_CSS_FONT_SIZE_X_LARGE:
        case LXB_CSS_FONT_SIZE_XX_LARGE:
        case LXB_CSS_FONT_SIZE_XXX_LARGE:
        case LXB_CSS_FONT_SIZE_MATH:
        case LXB_CSS_FONT_SIZE_LARGER:
        case LXB_CSS_FONT_SIZE_SMALLER:
            fs->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_float_reference(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_FLOAT_REFERENCE_INLINE:
        case LXB_CSS_FLOAT_REFERENCE_COLUMN:
        case LXB_CSS_FLOAT_REFERENCE_REGION:
        case LXB_CSS_FLOAT_REFERENCE_PAGE:
            declar->u.float_reference->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_float(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_float_t *fp = declar->u.floatp;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        if (token->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION) {
            goto snap;
        }

        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_FLOAT_BLOCK_START:
        case LXB_CSS_FLOAT_BLOCK_END:
        case LXB_CSS_FLOAT_INLINE_START:
        case LXB_CSS_FLOAT_INLINE_END:
        case LXB_CSS_FLOAT_SNAP_BLOCK:
        case LXB_CSS_FLOAT_SNAP_INLINE:
        case LXB_CSS_FLOAT_LEFT:
        case LXB_CSS_FLOAT_RIGHT:
        case LXB_CSS_FLOAT_TOP:
        case LXB_CSS_FLOAT_BOTTOM:
        case LXB_CSS_FLOAT_NONE:
            fp->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);

snap:

    type = lxb_css_value_by_name(lxb_css_syntax_token_function(token)->data,
                                 lxb_css_syntax_token_function(token)->length);

    if (type != LXB_CSS_FLOAT_SNAP_BLOCK
        && type != LXB_CSS_FLOAT_SNAP_INLINE)
    {
        return lxb_css_parser_failed(parser);
    }

    fp->type = type;

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_length(parser, token, &fp->length.length);
    if (!res) {
        return lxb_css_parser_failed(parser);
    }

    fp->length.type = LXB_CSS_VALUE__LENGTH;

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_COMMA) {
        if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
            fp->snap_type = LXB_CSS_VALUE__UNDEF;

            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);
        }

        return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        case LXB_CSS_FLOAT_START:
        case LXB_CSS_FLOAT_END:
            if (fp->type != LXB_CSS_FLOAT_SNAP_BLOCK) {
                return lxb_css_parser_failed(parser);
            }

            fp->snap_type = type;
            break;

        case LXB_CSS_FLOAT_LEFT:
        case LXB_CSS_FLOAT_RIGHT:
            if (fp->type != LXB_CSS_FLOAT_SNAP_INLINE) {
                return lxb_css_parser_failed(parser);
            }

            fp->snap_type = type;
            break;

        case LXB_CSS_FLOAT_NEAR:
            fp->snap_type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
        lxb_css_syntax_parser_consume(parser);

        return lxb_css_parser_success(parser);
    }

    return lxb_css_parser_failed(parser);
}

bool
lxb_css_property_state_clear(lxb_css_parser_t *parser,
                             const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_CLEAR_INLINE_START:
        case LXB_CSS_CLEAR_INLINE_END:
        case LXB_CSS_CLEAR_BLOCK_START:
        case LXB_CSS_CLEAR_BLOCK_END:
        case LXB_CSS_CLEAR_LEFT:
        case LXB_CSS_CLEAR_RIGHT:
        case LXB_CSS_CLEAR_TOP:
        case LXB_CSS_CLEAR_BOTTOM:
        case LXB_CSS_CLEAR_NONE:
            declar->u.clear->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_float_defer(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_float_defer_t *fd = declar->u.float_defer;

    res = lxb_css_property_state_integer(parser, token, &fd->integer);
    if (res) {
        fd->type = LXB_CSS_FLOAT_DEFER__INTEGER;

        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_FLOAT_DEFER_LAST:
        case LXB_CSS_FLOAT_DEFER_NONE:
            fd->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_float_offset(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_float_offset_t *fo = declar->u.float_offset;

    res = lxb_css_property_state_length_percentage(parser, token,
                                     (lxb_css_value_length_percentage_t *) fo);
    if (res) {
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            fo->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_wrap_flow(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_WRAP_FLOW_AUTO:
        case LXB_CSS_WRAP_FLOW_BOTH:
        case LXB_CSS_WRAP_FLOW_START:
        case LXB_CSS_WRAP_FLOW_END:
        case LXB_CSS_WRAP_FLOW_MINIMUM:
        case LXB_CSS_WRAP_FLOW_MAXIMUM:
        case LXB_CSS_WRAP_FLOW_CLEAR:
            declar->u.wrap_flow->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_wrap_through(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_WRAP_THROUGH_WRAP:
        case LXB_CSS_WRAP_THROUGH_NONE:
            declar->u.wrap_through->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_flex_direction(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_FLEX_DIRECTION_ROW:
        case LXB_CSS_FLEX_DIRECTION_ROW_REVERSE:
        case LXB_CSS_FLEX_DIRECTION_COLUMN:
        case LXB_CSS_FLEX_DIRECTION_COLUMN_REVERSE:
            declar->u.flex_direction->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_flex_wrap(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
            /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            /* Local. */
        case LXB_CSS_FLEX_WRAP_NOWRAP:
        case LXB_CSS_FLEX_WRAP_WRAP:
        case LXB_CSS_FLEX_WRAP_WRAP_REVERSE:
            declar->u.flex_wrap->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_flex_flow(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_flex_flow_t *ff = declar->u.flex_flow;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_FLEX_DIRECTION_ROW:
        case LXB_CSS_FLEX_DIRECTION_ROW_REVERSE:
        case LXB_CSS_FLEX_DIRECTION_COLUMN:
        case LXB_CSS_FLEX_DIRECTION_COLUMN_REVERSE:
            ff->type_direction = type;
            goto direction;

        case LXB_CSS_FLEX_WRAP_NOWRAP:
        case LXB_CSS_FLEX_WRAP_WRAP:
        case LXB_CSS_FLEX_WRAP_WRAP_REVERSE:
            ff->wrap = type;
            goto wrap;

        default:
            return lxb_css_parser_failed(parser);
    }

direction:

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_success(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        case LXB_CSS_FLEX_WRAP_NOWRAP:
        case LXB_CSS_FLEX_WRAP_WRAP:
        case LXB_CSS_FLEX_WRAP_WRAP_REVERSE:
            ff->wrap = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    goto done;

wrap:

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_success(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        case LXB_CSS_FLEX_DIRECTION_ROW:
        case LXB_CSS_FLEX_DIRECTION_ROW_REVERSE:
        case LXB_CSS_FLEX_DIRECTION_COLUMN:
        case LXB_CSS_FLEX_DIRECTION_COLUMN_REVERSE:
            ff->type_direction = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

done:

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

lxb_inline bool
lxb_css_property_state_flex_grow_shrink(lxb_css_parser_t *parser,
                                        const lxb_css_syntax_token_t *token,
                                        lxb_css_property_flex_t *flex)
{
    bool res;

    res = lxb_css_property_state_number(parser, token, &flex->grow.number);
    if (!res) {
        return false;
    }

    flex->grow.type = LXB_CSS_FLEX_GROW__NUMBER;

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    res = lxb_css_property_state_number(parser, token, &flex->shrink.number);
    if (res) {
        flex->shrink.type = LXB_CSS_FLEX_SHRINK__NUMBER;
    }

    return true;
}

lxb_inline bool
lxb_css_property_state_flex_grow_basis(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token,
                                       lxb_css_property_flex_t *flex)
{
    bool res;
    lxb_css_value_type_t type;

    res = lxb_css_property_state_width_handler(parser, token,
                               (lxb_css_property_flex_basis_t *) &flex->basis);
    if (res) {
        return true;
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return false;
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);

    if (type == LXB_CSS_FLEX_BASIS_CONTENT) {
        flex->basis.type = type;

        lxb_css_syntax_parser_consume(parser);
        return true;
    }

    return false;
}

lxb_inline void
lxb_css_property_state_flex_set_basis_zero_pct(lxb_css_property_flex_t *flex)
{
    flex->basis.type = LXB_CSS_VALUE__PERCENTAGE;
    flex->basis.u.percentage.num = 0.0;
    flex->basis.u.percentage.is_float = false;
}

bool
lxb_css_property_state_flex(lxb_css_parser_t *parser,
                            const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_flex_t *flex = declar->u.flex;

    res = lxb_css_property_state_flex_grow_shrink(parser, token, flex);

    if (res) {
        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        res = lxb_css_property_state_flex_grow_basis(parser, token, flex);

        if (!res && token->type != LXB_CSS_SYNTAX_TOKEN__END) {
            flex->basis.type = LXB_CSS_VALUE__NUMBER;
            flex->basis.u.length.num = flex->grow.number.num;
            flex->basis.u.length.unit = LXB_CSS_UNIT__UNDEF;
            flex->basis.u.length.is_float = flex->grow.number.is_float;

            flex->grow.type = LXB_CSS_VALUE__UNDEF;

            if (flex->shrink.type != LXB_CSS_VALUE__UNDEF) {
                flex->grow = flex->shrink;
                flex->shrink.type = LXB_CSS_VALUE__UNDEF;

                goto try_shrink_last;
            }

            res = lxb_css_property_state_flex_grow_shrink(parser, token, flex);
            if (!res) {
                return lxb_css_parser_failed(parser);
            }
        }
        else if (!res) {
            /* flex: <number> and flex: <number> <number> expand to
             * flex-basis: 0% per the shorthand grammar. */
            lxb_css_property_state_flex_set_basis_zero_pct(flex);
        }

        return lxb_css_parser_success(parser);
    }
    else {
        res = lxb_css_property_state_flex_grow_basis(parser, token, flex);

        if (res) {
            /* flex: <basis> expands to 1 1 <basis>; flex-basis longhand
             * remains available when the author needs only the basis. */
            flex->grow.type = LXB_CSS_FLEX_GROW__NUMBER;
            flex->grow.number.num = 1.0;
            flex->grow.number.is_float = false;
            flex->shrink.type = LXB_CSS_FLEX_SHRINK__NUMBER;
            flex->shrink.number.num = 1.0;
            flex->shrink.number.is_float = false;

            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);

            (void) lxb_css_property_state_flex_grow_shrink(parser, token, flex);

            return lxb_css_parser_success(parser);
        }
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_FLEX_NONE:
            flex->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);

try_shrink_last:

    res = lxb_css_property_state_number(parser, token, &flex->shrink.number);
    if (res) {
        flex->shrink.type = LXB_CSS_FLEX_SHRINK__NUMBER;
    }

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_flex_grow(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_flex_grow_t *fg = declar->u.flex_grow;

    res = lxb_css_property_state_number(parser, token, &fg->number);
    if (res) {
        if (fg->number.num < 0) {
            return lxb_css_parser_failed(parser);
        }

        fg->type = LXB_CSS_FLEX_GROW__NUMBER;

        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            fg->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_flex_shrink(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_flex_grow_t *fs = declar->u.flex_shrink;

    res = lxb_css_property_state_number(parser, token, &fs->number);
    if (res) {
        if (fs->number.num < 0) {
            return lxb_css_parser_failed(parser);
        }

        fs->type = LXB_CSS_FLEX_SHRINK__NUMBER;

        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            fs->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_flex_basis(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_flex_basis_t *fb = declar->u.flex_basis;

    res = lxb_css_property_state_width_handler(parser, token,
                                               (lxb_css_property_width_t *) fb);
    if (res) {
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        case LXB_CSS_FLEX_BASIS_CONTENT:
            fb->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_justify_content(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_VALUE_START:
        case LXB_CSS_VALUE_END:
        case LXB_CSS_JUSTIFY_CONTENT_FLEX_START:
        case LXB_CSS_JUSTIFY_CONTENT_FLEX_END:
        case LXB_CSS_JUSTIFY_CONTENT_CENTER:
        case LXB_CSS_JUSTIFY_CONTENT_SPACE_BETWEEN:
        case LXB_CSS_JUSTIFY_CONTENT_SPACE_AROUND:
        case LXB_CSS_JUSTIFY_CONTENT_SPACE_EVENLY:
            declar->u.justify_content->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_align_items(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_VALUE_START:
        case LXB_CSS_VALUE_END:
        case LXB_CSS_ALIGN_ITEMS_FLEX_START:
        case LXB_CSS_ALIGN_ITEMS_FLEX_END:
        case LXB_CSS_ALIGN_ITEMS_CENTER:
        case LXB_CSS_ALIGN_ITEMS_BASELINE:
        case LXB_CSS_ALIGN_ITEMS_STRETCH:
            declar->u.align_items->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_align_self(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_VALUE_START:
        case LXB_CSS_VALUE_END:
        case LXB_CSS_ALIGN_SELF_AUTO:
        case LXB_CSS_ALIGN_SELF_FLEX_START:
        case LXB_CSS_ALIGN_SELF_FLEX_END:
        case LXB_CSS_ALIGN_SELF_CENTER:
        case LXB_CSS_ALIGN_SELF_BASELINE:
        case LXB_CSS_ALIGN_SELF_STRETCH:
            declar->u.align_self->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_align_content(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_VALUE_START:
        case LXB_CSS_VALUE_END:
        case LXB_CSS_ALIGN_CONTENT_FLEX_START:
        case LXB_CSS_ALIGN_CONTENT_FLEX_END:
        case LXB_CSS_ALIGN_CONTENT_CENTER:
        case LXB_CSS_ALIGN_CONTENT_SPACE_BETWEEN:
        case LXB_CSS_ALIGN_CONTENT_SPACE_AROUND:
        case LXB_CSS_ALIGN_CONTENT_SPACE_EVENLY:
        case LXB_CSS_ALIGN_CONTENT_STRETCH:
            declar->u.align_content->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_dominant_baseline(lxb_css_parser_t *parser,
                                         const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_DOMINANT_BASELINE_AUTO:
        case LXB_CSS_DOMINANT_BASELINE_TEXT_BOTTOM:
        case LXB_CSS_DOMINANT_BASELINE_ALPHABETIC:
        case LXB_CSS_DOMINANT_BASELINE_IDEOGRAPHIC:
        case LXB_CSS_DOMINANT_BASELINE_MIDDLE:
        case LXB_CSS_DOMINANT_BASELINE_CENTRAL:
        case LXB_CSS_DOMINANT_BASELINE_MATHEMATICAL:
        case LXB_CSS_DOMINANT_BASELINE_HANGING:
        case LXB_CSS_DOMINANT_BASELINE_TEXT_TOP:
            declar->u.dominant_baseline->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_alignment_baseline_h(lxb_css_parser_t *parser,
                                            const lxb_css_syntax_token_t *token,
                                            lxb_css_property_alignment_baseline_t *ab)
{
    lxb_css_value_type_t type;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return false;
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        case LXB_CSS_ALIGNMENT_BASELINE_BASELINE:
        case LXB_CSS_ALIGNMENT_BASELINE_TEXT_BOTTOM:
        case LXB_CSS_ALIGNMENT_BASELINE_ALPHABETIC:
        case LXB_CSS_ALIGNMENT_BASELINE_IDEOGRAPHIC:
        case LXB_CSS_ALIGNMENT_BASELINE_MIDDLE:
        case LXB_CSS_ALIGNMENT_BASELINE_CENTRAL:
        case LXB_CSS_ALIGNMENT_BASELINE_MATHEMATICAL:
        case LXB_CSS_ALIGNMENT_BASELINE_TEXT_TOP:
            ab->type = type;

            lxb_css_syntax_parser_consume(parser);
            return true;

        default:
            return false;
    }
}

bool
lxb_css_property_state_baseline_shift_h(lxb_css_parser_t *parser,
                                        const lxb_css_syntax_token_t *token,
                                        lxb_css_property_baseline_shift_t *bs)
{
    bool res;
    lxb_css_value_type_t type;

    res = lxb_css_property_state_length_percentage(parser, token, bs);

    if (res) {
        return true;
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return false;
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        case LXB_CSS_BASELINE_SHIFT_SUB:
        case LXB_CSS_BASELINE_SHIFT_SUPER:
        case LXB_CSS_BASELINE_SHIFT_TOP:
        case LXB_CSS_BASELINE_SHIFT_CENTER:
        case LXB_CSS_BASELINE_SHIFT_BOTTOM:
            bs->type = type;

            lxb_css_syntax_parser_consume(parser);
            return true;

        default:
            return false;
    }
}

bool
lxb_css_property_state_vertical_align(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    uint8_t maps;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_vertical_align_t *va = declar->u.vertical_align;

    maps = 0;

again:

    res = lxb_css_property_state_alignment_baseline_h(parser, token,
                                                      &va->alignment);
    if (res) {
        if (maps & 1 << 1) {
            return lxb_css_parser_failed(parser);
        }

        maps |= 1 << 1;

        token = lxb_css_syntax_parser_token_wo_ws(parser);
        lxb_css_property_state_check_token(parser, token);

        res = lxb_css_property_state_baseline_shift_h(parser, token,
                                                      &va->shift);
        if (res) {
            if (maps & 1 << 2) {
                return lxb_css_parser_failed(parser);
            }

            maps |= 1 << 2;

            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);
        }
    }
    else {
        res = lxb_css_property_state_baseline_shift_h(parser, token,
                                                      &va->shift);
        if (res) {
            if (maps & 1 << 2) {
                return lxb_css_parser_failed(parser);
            }

            maps |= 1 << 2;

            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);

            res = lxb_css_property_state_alignment_baseline_h(parser, token,
                                                              &va->alignment);
            if (res) {
                if (maps & 1 << 1) {
                    return lxb_css_parser_failed(parser);
                }

                maps |= 1 << 1;

                token = lxb_css_syntax_parser_token_wo_ws(parser);
                lxb_css_property_state_check_token(parser, token);
            }
        }
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        if (maps != 0) {
            return lxb_css_parser_success(parser);
        }

        return lxb_css_parser_failed(parser);
    }

    if (maps & 1 << 3) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_VERTICAL_ALIGN_FIRST:
        case LXB_CSS_VERTICAL_ALIGN_LAST:
            va->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    token = lxb_css_syntax_parser_token_wo_ws(parser);
    lxb_css_property_state_check_token(parser, token);

    maps = 1 << 3;

    goto again;
}

bool
lxb_css_property_state_baseline_source(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_BASELINE_SOURCE_AUTO:
        case LXB_CSS_BASELINE_SOURCE_FIRST:
        case LXB_CSS_BASELINE_SOURCE_LAST:
            declar->u.baseline_source->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_alignment_baseline(lxb_css_parser_t *parser,
                                          const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_ALIGNMENT_BASELINE_BASELINE:
        case LXB_CSS_ALIGNMENT_BASELINE_TEXT_BOTTOM:
        case LXB_CSS_ALIGNMENT_BASELINE_ALPHABETIC:
        case LXB_CSS_ALIGNMENT_BASELINE_IDEOGRAPHIC:
        case LXB_CSS_ALIGNMENT_BASELINE_MIDDLE:
        case LXB_CSS_ALIGNMENT_BASELINE_CENTRAL:
        case LXB_CSS_ALIGNMENT_BASELINE_MATHEMATICAL:
        case LXB_CSS_ALIGNMENT_BASELINE_TEXT_TOP:
            declar->u.alignment_baseline->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_baseline_shift(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    res = lxb_css_property_state_length_percentage(parser, token,
                                                   declar->u.baseline_shift);
    if (res) {
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_BASELINE_SHIFT_SUB:
        case LXB_CSS_BASELINE_SHIFT_SUPER:
        case LXB_CSS_BASELINE_SHIFT_TOP:
        case LXB_CSS_BASELINE_SHIFT_CENTER:
        case LXB_CSS_BASELINE_SHIFT_BOTTOM:
            declar->u.baseline_shift->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_line_height(lxb_css_parser_t *parser,
                                   const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    res = lxb_css_property_state_number_length_percentage(parser, token,
                                                          declar->u.line_height);
    if (res) {
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_LINE_HEIGHT_NORMAL:
            declar->u.line_height->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_z_index(lxb_css_parser_t *parser,
                               const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    res = lxb_css_property_state_integer(parser, token,
                                         &declar->u.z_index->integer);
    if (res) {
        declar->u.z_index->type = LXB_CSS_VALUE__INTEGER;
        return lxb_css_parser_success(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_Z_INDEX_AUTO:
            declar->u.line_height->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_direction(lxb_css_parser_t *parser,
                                 const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_DIRECTION_LTR:
        case LXB_CSS_DIRECTION_RTL:
            declar->u.direction->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_unicode_bidi(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_UNICODE_BIDI_NORMAL:
        case LXB_CSS_UNICODE_BIDI_EMBED:
        case LXB_CSS_UNICODE_BIDI_ISOLATE:
        case LXB_CSS_UNICODE_BIDI_BIDI_OVERRIDE:
        case LXB_CSS_UNICODE_BIDI_ISOLATE_OVERRIDE:
        case LXB_CSS_UNICODE_BIDI_PLAINTEXT:
            declar->u.unicode_bidi->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_writing_mode(lxb_css_parser_t *parser,
                                    const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_WRITING_MODE_HORIZONTAL_TB:
        case LXB_CSS_WRITING_MODE_VERTICAL_RL:
        case LXB_CSS_WRITING_MODE_VERTICAL_LR:
        case LXB_CSS_WRITING_MODE_SIDEWAYS_RL:
        case LXB_CSS_WRITING_MODE_SIDEWAYS_LR:
            declar->u.writing_mode->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_text_orientation(lxb_css_parser_t *parser,
                                        const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_TEXT_ORIENTATION_MIXED:
        case LXB_CSS_TEXT_ORIENTATION_UPRIGHT:
        case LXB_CSS_TEXT_ORIENTATION_SIDEWAYS:
            declar->u.text_orientation->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_text_combine_upright(lxb_css_parser_t *parser,
                                            const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_text_combine_upright_t *tcu = declar->u.text_combine_upright;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_TEXT_COMBINE_UPRIGHT_NONE:
        case LXB_CSS_TEXT_COMBINE_UPRIGHT_ALL:
            tcu->type = type;
            break;

        case LXB_CSS_TEXT_COMBINE_UPRIGHT_DIGITS:
            tcu->type = type;

            lxb_css_syntax_parser_consume(parser);
            token = lxb_css_syntax_parser_token_wo_ws(parser);
            lxb_css_property_state_check_token(parser, token);

            res = lxb_css_property_state_integer(parser, token,
                                                 &tcu->digits);
            if (res) {
                if (tcu->digits.num != 2 && tcu->digits.num != 4) {
                    return lxb_css_parser_failed(parser);
                }
            }

            return lxb_css_parser_success(parser);

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_overflow_x(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_OVERFLOW_X_VISIBLE:
        case LXB_CSS_OVERFLOW_X_HIDDEN:
        case LXB_CSS_OVERFLOW_X_CLIP:
        case LXB_CSS_OVERFLOW_X_SCROLL:
        case LXB_CSS_OVERFLOW_X_AUTO:
            declar->u.overflow_x->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_overflow_y(lxb_css_parser_t *parser,
                                  const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_overflow_x(parser, token, ctx);
}

bool
lxb_css_property_state_overflow(lxb_css_parser_t *parser,
                                const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_OVERFLOW_VISIBLE:
        case LXB_CSS_OVERFLOW_HIDDEN:
        case LXB_CSS_OVERFLOW_CLIP:
        case LXB_CSS_OVERFLOW_SCROLL:
        case LXB_CSS_OVERFLOW_AUTO:
            declar->u.overflow->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_overflow_block(lxb_css_parser_t *parser,
                                      const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_overflow_x(parser, token, ctx);
}

bool
lxb_css_property_state_overflow_inline(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_overflow_x(parser, token, ctx);
}

bool
lxb_css_property_state_text_overflow(lxb_css_parser_t *parser,
                                     const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_TEXT_OVERFLOW_CLIP:
        case LXB_CSS_TEXT_OVERFLOW_ELLIPSIS:
            declar->u.text_overflow->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

lxb_status_t
lxb_css_property_state_text_decoration_line_h(lxb_css_parser_t *parser,
                                              const lxb_css_syntax_token_t *token,
                                              lxb_css_property_text_decoration_line_t *tdl)
{
    lxb_css_value_type_t type;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return LXB_STATUS_NEXT;
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        case LXB_CSS_TEXT_DECORATION_LINE_NONE:
            tdl->type = type;

            lxb_css_syntax_parser_consume(parser);
            return LXB_STATUS_OK;

        default:
            goto first;
    }

next:

    lxb_css_syntax_parser_consume(parser);
    token = lxb_css_syntax_parser_token_wo_ws(parser);
    if (token == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return LXB_STATUS_OK;
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);

first:

    switch (type) {
        case LXB_CSS_TEXT_DECORATION_LINE_UNDERLINE:
            if (tdl->underline != LXB_CSS_VALUE__UNDEF) {
                return LXB_STATUS_STOP;
            }

            tdl->underline = type;
            goto next;

        case LXB_CSS_TEXT_DECORATION_LINE_OVERLINE:
            if (tdl->overline != LXB_CSS_VALUE__UNDEF) {
                return LXB_STATUS_STOP;
            }

            tdl->overline = type;
            goto next;

        case LXB_CSS_TEXT_DECORATION_LINE_LINE_THROUGH:
            if (tdl->line_through != LXB_CSS_VALUE__UNDEF) {
                return LXB_STATUS_STOP;
            }

            tdl->line_through = type;
            goto next;

        case LXB_CSS_TEXT_DECORATION_LINE_BLINK:
            if (tdl->blink != LXB_CSS_VALUE__UNDEF) {
                return LXB_STATUS_STOP;
            }

            tdl->blink = type;
            goto next;

        default:
            if (tdl->underline != LXB_CSS_VALUE__UNDEF
                || tdl->overline != LXB_CSS_VALUE__UNDEF
                || tdl->line_through != LXB_CSS_VALUE__UNDEF
                || tdl->blink != LXB_CSS_VALUE__UNDEF)
            {
                return LXB_STATUS_OK;
            }

            return LXB_STATUS_NEXT;
    }
}

bool
lxb_css_property_state_text_decoration_line(lxb_css_parser_t *parser,
                                            const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_status_t status;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_text_decoration_line_t *tdl = declar->u.text_decoration_line;

    status = lxb_css_property_state_text_decoration_line_h(parser, token, tdl);

    if (status == LXB_STATUS_OK) {
        return lxb_css_parser_success(parser);
    }
    else if (status == LXB_STATUS_STOP) {
        return lxb_css_parser_failed(parser);
    }
    else if (status != LXB_STATUS_NEXT) {
        return lxb_css_parser_memory_fail(parser);
    }

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            tdl->type = type;

            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);

        default:
            return lxb_css_parser_failed(parser);
    }
}

bool
lxb_css_property_state_text_decoration_style_h(lxb_css_parser_t *parser,
                            const lxb_css_syntax_token_t *token,
                            lxb_css_property_text_decoration_style_t *tds)
{
    lxb_css_value_type_t type;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return false;
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        case LXB_CSS_TEXT_DECORATION_STYLE_SOLID:
        case LXB_CSS_TEXT_DECORATION_STYLE_DOUBLE:
        case LXB_CSS_TEXT_DECORATION_STYLE_DOTTED:
        case LXB_CSS_TEXT_DECORATION_STYLE_DASHED:
        case LXB_CSS_TEXT_DECORATION_STYLE_WAVY:
            tds->type = type;

            lxb_css_syntax_parser_consume(parser);
            return true;

        default:
            return false;
    }
}

bool
lxb_css_property_state_text_decoration_style(lxb_css_parser_t *parser,
                                             const lxb_css_syntax_token_t *token, void *ctx)
{
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        return lxb_css_parser_failed(parser);
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
        /* Local. */
        case LXB_CSS_TEXT_DECORATION_STYLE_SOLID:
        case LXB_CSS_TEXT_DECORATION_STYLE_DOUBLE:
        case LXB_CSS_TEXT_DECORATION_STYLE_DOTTED:
        case LXB_CSS_TEXT_DECORATION_STYLE_DASHED:
        case LXB_CSS_TEXT_DECORATION_STYLE_WAVY:
            declar->u.text_decoration_style->type = type;
            break;

        default:
            return lxb_css_parser_failed(parser);
    }

    lxb_css_syntax_parser_consume(parser);

    return lxb_css_parser_success(parser);
}

bool
lxb_css_property_state_text_decoration_color(lxb_css_parser_t *parser,
                                             const lxb_css_syntax_token_t *token, void *ctx)
{
    return lxb_css_property_state_color(parser, token, ctx);
}

bool
lxb_css_property_state_text_decoration(lxb_css_parser_t *parser,
                                       const lxb_css_syntax_token_t *token, void *ctx)
{
    bool res, line, style, color;
    lxb_status_t status;
    lxb_css_value_type_t type;
    lxb_css_rule_declaration_t *declar = ctx;
    lxb_css_property_text_decoration_t *td = declar->u.text_decoration;

    if (token->type != LXB_CSS_SYNTAX_TOKEN_IDENT) {
        goto lsc;
    }

    type = lxb_css_value_by_name(lxb_css_syntax_token_ident(token)->data,
                                 lxb_css_syntax_token_ident(token)->length);
    switch (type) {
        /* Global. */
        case LXB_CSS_VALUE_INITIAL:
        case LXB_CSS_VALUE_INHERIT:
        case LXB_CSS_VALUE_UNSET:
        case LXB_CSS_VALUE_REVERT:
            td->line.type = type;

            lxb_css_syntax_parser_consume(parser);
            return lxb_css_parser_success(parser);

        default:
            break;
    }

lsc:

    line = false;
    style = false;
    color = false;

    for (size_t i = 0; i < 3; i++) {
        if (!line) {
            status = lxb_css_property_state_text_decoration_line_h(parser, token,
                                                                   &td->line);
            if (status == LXB_STATUS_OK) {
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                lxb_css_property_state_check_token(parser, token);

                line = true;
            }
            else if (status == LXB_STATUS_STOP) {
                return lxb_css_parser_failed(parser);
            }
            else if (status != LXB_STATUS_NEXT) {
                return lxb_css_parser_memory_fail(parser);
            }
        }

        if (!style) {
            res = lxb_css_property_state_text_decoration_style_h(parser, token,
                                                                 &td->style);
            if (res) {
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                lxb_css_property_state_check_token(parser, token);

                style = true;
            }
        }

        if (!color) {
            res = lxb_css_property_state_color_handler(parser, token,
                                         (lxb_css_value_color_t *) &td->color,
                                         &status);
            if (res) {
                token = lxb_css_syntax_parser_token_wo_ws(parser);
                lxb_css_property_state_check_token(parser, token);

                color = true;
            }
            else {
                if (status != LXB_STATUS_OK) {
                    return lxb_css_parser_failed(parser);
                }
            }
        }
    }

    if (!line && !style && !color) {
        return lxb_css_parser_failed(parser);
    }

    return lxb_css_parser_success(parser);
}
