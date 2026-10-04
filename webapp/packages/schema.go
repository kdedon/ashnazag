package packages

import (
	"bytes"
	"embed"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"reflect"
	"regexp"
	"strings"
	"unicode/utf8"
)

//go:embed *.schema.json fixtures/ibrowse.json
var schemas embed.FS

func Schema(kind string) ([]byte, error) {
	switch kind {
	case "recipe", "lock", "receipt":
		return schemas.ReadFile(kind + ".schema.json")
	default:
		return nil, fmt.Errorf("unknown schema %q", kind)
	}
}
func IBrowse() (Recipe, error) {
	b, err := schemas.ReadFile("fixtures/ibrowse.json")
	if err != nil {
		return Recipe{}, err
	}
	return DecodeRecipe(b)
}

// ValidateJSON implements the keywords used by the embedded schemas.
func ValidateJSON(kind string, data []byte) error {
	b, err := Schema(kind)
	if err != nil {
		return err
	}
	return ValidateSchema(b, data)
}

// ValidateSchema checks data against a schema using the same keyword subset.
func ValidateSchema(b, data []byte) error {
	if !utf8.Valid(data) {
		return fmt.Errorf("JSON must be valid UTF-8")
	}
	var schema map[string]any
	if err := json.Unmarshal(b, &schema); err != nil {
		return err
	}
	var v any
	d := json.NewDecoder(bytes.NewReader(data))
	if err := d.Decode(&v); err != nil {
		return err
	}
	var extra any
	if err := d.Decode(&extra); err != io.EOF {
		return fmt.Errorf("trailing JSON")
	}
	return validate(schema, schema, v, "$")
}
func validate(root, s map[string]any, v any, at string) error {
	fail := func(reason string) error { return fmt.Errorf("%s: %s", at, reason) }
	if ref, ok := s["$ref"].(string); ok {
		key := strings.TrimPrefix(ref, "#/$defs/")
		defs := root["$defs"].(map[string]any)
		target, ok := defs[key].(map[string]any)
		if !ok {
			return fail("unknown schema reference")
		}
		return validate(root, target, v, at)
	}
	if c, ok := s["const"]; ok && !reflect.DeepEqual(c, v) {
		return fail("wrong constant")
	}
	if list, ok := s["enum"].([]any); ok {
		found := false
		for _, e := range list {
			found = found || reflect.DeepEqual(e, v)
		}
		if !found {
			return fail("invalid enum value")
		}
	}
	switch s["type"] {
	case "object":
		obj, ok := v.(map[string]any)
		if !ok {
			return fail("expected object")
		}
		props := s["properties"].(map[string]any)
		if required, ok := s["required"].([]any); ok {
			for _, k := range required {
				if _, ok := obj[k.(string)]; !ok {
					return fail("missing " + k.(string))
				}
			}
		}
		for k, value := range obj {
			p, ok := props[k]
			if !ok {
				return fail("unknown property " + k)
			}
			if err := validate(root, p.(map[string]any), value, at+"."+k); err != nil {
				return err
			}
		}
	case "array":
		a, ok := v.([]any)
		if !ok {
			return fail("expected array")
		}
		seen := map[string]bool{}
		for i, item := range a {
			if s["uniqueItems"] == true {
				b, _ := json.Marshal(item)
				if seen[string(b)] {
					return fail("duplicate array item")
				}
				seen[string(b)] = true
			}
			if err := validate(root, s["items"].(map[string]any), item, fmt.Sprintf("%s[%d]", at, i)); err != nil {
				return err
			}
		}
	case "string":
		str, ok := v.(string)
		if !ok {
			return fail("expected string")
		}
		n := len([]rune(str))
		if min, ok := s["minLength"].(float64); ok && n < int(min) {
			return fail("string too short")
		}
		if max, ok := s["maxLength"].(float64); ok && n > int(max) {
			return fail("string too long")
		}
		if pattern, ok := s["pattern"].(string); ok {
			matched, err := regexp.MatchString(pattern, str)
			if err != nil {
				return err
			}
			if !matched {
				return fail("invalid string")
			}
		}
	case "integer":
		n, ok := v.(float64)
		if !ok || math.Trunc(n) != n || math.Abs(n) > 9007199254740991 {
			return fail("expected exact integer")
		}
		if min, ok := s["minimum"].(float64); ok && n < min {
			return fail("integer too small")
		}
		if max, ok := s["maximum"].(float64); ok && n > max {
			return fail("integer too large")
		}
	case "boolean":
		if _, ok := v.(bool); !ok {
			return fail("expected boolean")
		}
	}
	return nil
}
