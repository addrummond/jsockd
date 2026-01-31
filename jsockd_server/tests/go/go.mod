module github.com/addrummond/jsockd/jsockd_server/tests/go

go 1.22.0

replace github.com/addrummond/jsockd/clients/go/jsockdclient => ../../../clients/go/jsockdclient

// __jsockd_version_check__
require github.com/addrummond/jsockd/clients/go/jsockdclient v0.0.142
