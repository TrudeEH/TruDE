#include "seth.h"
#include <string.h>

/* The bundled tools and elicitation forms use this bounded JSON Schema subset.
 * Arbitrary external schemas are passed through unchanged to the provider. */
static int validate(J *schema, J *value, const char *path, int depth) {
    if (depth > 64)
        return fail("Schema nesting too deep");
    if (!schema)
        return 0;
    const char *type = gs(schema, "type");
    int valid = !*type || (!strcmp(type, "object") && value && value->type == JOBJ) ||
                (!strcmp(type, "array") && value && value->type == JARR) ||
                (!strcmp(type, "string") && value && value->type == JSTR) ||
                (!strcmp(type, "number") && value && value->type == JNUM) ||
                (!strcmp(type, "integer") && value && value->type == JNUM &&
                 value->n >= -9007199254740991.0 && value->n <= 9007199254740991.0 &&
                 value->n == (int64_t)value->n) ||
                (!strcmp(type, "boolean") && value && value->type == JBOOL) ||
                (!strcmp(type, "null") && value && value->type == JNULL);
    if (!valid)
        return fail("%s must be %s", path, type);
    J *en = jg(schema, "enum");
    if (en) {
        int found = 0;
        for (size_t i = 0; i < en->len; i++)
            found |= jeq(value, en->v[i]);
        if (!found)
            return fail("%s must match a listed choice", path);
    }
    if (value && value->type == JOBJ) {
        J *required = jg(schema, "required"), *props = jg(schema, "properties");
        for (size_t i = 0; required && i < required->len; i++)
            if (!jg(value, jstr(required->v[i])))
                return fail("%s.%s is required", path, jstr(required->v[i]));
        for (size_t i = 0; i < value->len; i++) {
            J *property = jg(props, value->v[i]->key);
            char *name = fmt("%s.%s", path, value->v[i]->key);
            int r = validate(property, value->v[i], name, depth + 1);
            free(name);
            if (r)
                return -1;
        }
    } else if (value && value->type == JARR) {
        J *items = jg(schema, "items");
        for (size_t i = 0; i < value->len; i++)
            if (validate(items, value->v[i], path, depth + 1))
                return -1;
    } else if (value && value->type == JSTR) {
        size_t n = strlen(value->s);
        if (n < gn(schema, "minLength", 0) || n > gn(schema, "maxLength", LIMIT))
            return fail("%s has an invalid length", path);
    } else if (value && value->type == JNUM) {
        if (jg(schema, "minimum") && value->n < gn(schema, "minimum", 0))
            return fail("%s is below its minimum", path);
        if (jg(schema, "maximum") && value->n > gn(schema, "maximum", 0))
            return fail("%s exceeds its maximum", path);
    }
    return 0;
}
int validate_schema(J *schema, J *value) {
    return validate(schema, value, "Input", 0);
}
