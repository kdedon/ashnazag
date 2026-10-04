package main

import (
	"amigaux.org/imagebuilder/planner"
	"encoding/json"
	"fmt"
	"io"
	"os"
)

func main() {
	data, err := io.ReadAll(io.LimitReader(os.Stdin, 1024*1024+1))
	if err != nil || len(data) > 1024*1024 {
		fmt.Fprintln(os.Stderr, "request must be readable and at most 1 MiB")
		os.Exit(1)
	}
	result := planner.Handle(data)
	fmt.Println(string(result))
	var response planner.Response
	if json.Unmarshal(result, &response) != nil || !response.OK {
		os.Exit(1)
	}
}
