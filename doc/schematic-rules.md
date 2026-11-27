# Schematic rule reference

Each node definition in a WaterWall schematic has a `rules` array and a
`graph_rules` array. Use empty arrays when a node has no checks of that kind.
Wave runs `rules` after validating the field definitions in the node editor's
structured form. Reserve `graph_rules` for constraints that apply when saving a
whole configuration, including configurations entered as JSON. A graph rule
must handle missing settings and unexpected value types because a JSON import
can bypass the structured form.

## Assertions and scope

A rule has an `assert` expression and a `message`. Wave stops at the first failed
assertion and displays its message. An optional `when` expression makes the
assertion conditional. Both expressions use the current settings object and
must return the Boolean `true` to pass.

A TCP listener needs exactly one of its two port settings. The following rule
compares their presence so it works with either a single port or a port list.

```json
{
  "assert": ["ne", ["present", "/port"], ["present", "/port-range"]],
  "message": "Choose a listen port, a port list or a port range."
}
```

An entry can contain a nested `rules` array instead of an assertion. Its
optional `at` expression changes the context to a selected value. An `each`
expression runs the nested rules against each item of a selected array. Use
`at` or `each` on the entry. The `when` expression is evaluated in the enclosing
context before any change of scope. An `each` entry skips missing and non-array
values, so use a separate shape or size assertion when the array is required.

This example checks each API client's token. The nested expressions read
`/token` relative to that client.

```json
{
  "each": ["get", "/clients"],
  "rules": [
    {
      "assert": [
        "all",
        ["eq", ["size", ["get", "/token"]], 64],
        ["format", ["get", "/token"], "hex"]
      ],
      "message": "Each API client needs a token containing exactly 64 hexadecimal digits."
    }
  ]
}
```

## Expressions

Use JSON scalar literals or an array whose first element names an operator.
Operands are expressions unless the operator requires a literal JSON Pointer,
type name, format name, or mapping object.

| Operator and operands | Result |
| --- | --- |
| `get, pointer, optionalFallback` | Value at a literal JSON Pointer. An empty pointer selects the current context. Missing values, including array indices outside the current array, return the fallback or `null`. An explicit JSON `null` remains `null`. |
| `present, pointer` | Whether the literal JSON Pointer resolves, including explicit `null` values. |
| `type, value, kind` | Whether the value has kind `null`, `boolean`, `string`, `number`, `integer`, `integral`, `array`, or `object`. `integer` requires an integer JSON representation. `integral` also accepts a whole-valued number such as `1.0`. |
| `eq, left, right` and `ne, left, right` | Equality or inequality of JSON values. |
| `lt, left, right`, `le, left, right`, `gt, left, right`, `ge, left, right` | Ordered comparison of two numbers. Returns `false` for other operand types. |
| `in, value, candidate, ...` | Whether the value equals any of the candidates. |
| `all, condition, ...` and `any, condition, ...` | Whether all or any conditions return `true`. |
| `not, condition` | Whether the condition fails to return `true`. |
| `if, condition, whenTrue, otherwise` | Evaluates the chosen branch. |
| `size, value` | Array length, object member count, or UTF-8 byte length of a string. Returns `0` for `null` and `1` for other scalars. |
| `lower, value` | ASCII lowercase string, or `""` for a non-string. |
| `list, value` | The original array or a one-item array containing the value. |
| `item, array, index` | Array element at a zero-based integer index, or `null` when unavailable. |
| `map, value, mapping, fallback` | Looks up a string in a literal mapping object, then returns that value or the evaluated fallback. |
| `add, left, right, ...`, `sub, left, right`, `mul, left, right`, `mod, left, right` | Numeric sum, difference, product, or remainder. Returns `null` when the operation is undefined. |
| `decimal-places, value, places` | Whether rounding to the specified number of decimal places preserves the value. |
| `every, array, condition` and `some, array, condition` | Evaluates a condition in each item's context. Returns whether all or any items pass, respectively. |
| `distinct, array, expression` | Whether all analyzed values are distinct. Evaluates the expression in each item's context. |
| `increasing, array, expression` | Whether the numeric values obtained from each item are strictly increasing. |
| `sum, array, expression` | Sums the numeric value obtained from each item. A nonnumeric item result returns `null`. |
| `format, value, name, optionalByteCount` | Whether a string matches a built-in format. |
| `starts-with, value, prefix` | Whether the string starts with the supplied prefix. |
| `substring, value, offset` | Suffix beginning at a zero-based UTF-8 byte offset, or `null` for an unavailable offset. |

The `format` names are `ipv4`, `ipv6`, `ipv4-cidr`, `ip-endpoint`, `hex`,
`base64`, and `nonnegative-int32`. An IP endpoint accepts numeric IP addresses
and optional ports. Enclose the IPv6 address in brackets when adding a port.
`hex` checks characters only, so combine it with a size assertion when a value
needs a specific number of digits. `base64` requires an additional integer
operand for the exact decoded byte count and validates canonical padding.

Use field definitions for ordinary input types, ranges, and required fields.
Use rules for conditions involving multiple fields, collection-wide checks,
and formats beyond the field kind. Test the omitted, boundary, and malformed
cases for each rule. Verify that both an accepted and a rejected value produce
the expected result in the relevant form or graph save.
