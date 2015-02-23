json_fdw
========

This PostgreSQL extension implements a Foreign Data Wrapper (FDW) for JSON
files. The extension doesn't require any data to be loaded into the database,
and supports analytic queries against array types, nested fields, and
heterogeneous documents.

json\_fdw currently only works with PostgreSQL 9.2, and uses YAJL to parse JSON
files. Future releases of this wrapper will use the JSON parser functions that
are to going to be introduced in the PostgreSQL 9.3 release.

This version of json\_fdw has been extended to be able to pull files from http
servers, stage them locally for the duration of the operation, and then delete
afterwards. It depends on libcurl from http://curl.haxx.se/libcurl/ . A future
TODO will use the ETAG mechanism to do long term local caching of the remote
content.
Also, this, and the original version, both are compatible with PostgreSQL 9.4
release.


Building
--------

json\_fdw depends on yajl-2.0 for parsing, and zlib-devel to read compressed
files, and libcurl to fetch files from http servers.
So we need to install these packages first:

NB. The following intructions have not been updated to reflect the libcurl dependancy
You'll need to adjust accordingly.

    ## Fedora 17+
    sudo yum install zlib-devel yajl-devel

    ## Ubuntu 12.10+
    sudo apt-get update
    sudo apt-get install zlib1g-dev libyajl-dev

    ## Other Linux Distributions
    (First install zlib-devel, cmake, and ruby)
    wget http://github.com/lloyd/yajl/tarball/2.0.1 -O yajl-2.0.1.tar.gz
    tar -xzvf yajl-2.0.1.tar.gz
    cd lloyd-yajl-f4b2b1a
    ./configure
    make
    sudo make install
    echo "/usr/local/lib" | sudo tee /etc/ld.so.conf.d/libyajl.conf
    sudo ldconfig

Once you have yajl-2.0 and zlib installed on your machine, you are ready to build
json\_fdw. For this, you need to include the pg\_config directory path in your
make command. This path is typically the same as your PostgreSQL installation's
bin/ directory path. For example:

    PATH=/usr/local/pgsql/bin/:$PATH make
    sudo PATH=/usr/local/pgsql/bin/:$PATH make install

**Note**: In RedHat 5.X and CentOS 5.X you may need to edit the Makefile and change "-l:libyajl.so.2" to "-lyajl".

Usage
-----

These two parameters can be set on a JSON foreign table object.

 * filename: The absolute path of a json file or a gzipped json file.
 * max\_error\_count: Maximum number of invalid json documents to skip before
   erroring out. Defaults to 0.

As an example, we demonstrate querying a compressed JSON file from scratch
here. We note that the underlying file contains JSON documents separated by
newlines, and that no data needs to be loaded into the database. Let's now start
with downloading the file.

    wget http://examples.citusdata.com/customer_reviews_nested_1998.json.gz

Next, let's log into Postgres, and run the following commands to create a
foreign table associated with this JSON file.

    -- load extension first time after install
    CREATE EXTENSION json_fdw;

    -- create server object
    CREATE SERVER json_server FOREIGN DATA WRAPPER json_fdw;

    -- create foreign table
    CREATE FOREIGN TABLE customer_reviews
    (
        customer_id TEXT,
        "review.date" DATE,
        "review.rating" INTEGER,
        "product.id" CHAR(10),
        "product.group" TEXT,
        "product.title" TEXT,
        "product.similar_ids" CHAR(10)[]
    )
    SERVER json_server
    OPTIONS (filename '/home/citusdata/customer_reviews_nested_1998.json.gz');

    -- optionally, collect data distribution statistics
    ANALYZE customer_reviews;

Finally, let's run some example SQL queries on your JSON file.

    -- find all reviews a particular customer made on the Dune series in 1998

    SELECT
        customer_id, "review.rating", "product.id", "product.title"
    FROM
        customer_reviews
    WHERE
        customer_id ='A27T7HVDXA3K2A' AND
        "product.title" LIKE '%Dune%' AND
        "review.date" >= '1998-01-01' AND
        "review.date" <= '1998-12-31';

    -- do we have a correlation between a book's title's length and its review ratings?

    SELECT
        width_bucket(length("product.title"), 1, 50, 5) title_length_bucket,
        round(avg("review.rating"), 2) AS review_average,
        count(*)
    FROM
        customer_reviews
    WHERE
        "product.group" = 'Book'
    GROUP BY
        title_length_bucket
    ORDER BY
        title_length_bucket;


Table Schema Conventions
------------------------

There are three things worth noting about table schemas. First, nested fields in
JSON documents are referenced using dot separators. For example, a field defined
as "review": { "rating" : 5 } in a JSON document is declared as "review.rating"
in the foreign table schema. The quotes around "review.rating" are necessary, as
identifiers that include dots aren't valid in Postgres otherwise.

Second, the foreign table schema is defined at read-time. If you have an
additional field that you'd like to query, such as "review.votes", you can
simply add the column name and start querying for data. You can even create
multiple table schemas for the same underlying file, and query through them.

Third, json\_fdw assumes that underlying data can be heterogeneous. If you are
querying for a column, and this field doesn't exist in a document, or the
field's data type doesn't match the declared column type, json\_fdw considers
that particular field to be null.


Querying Multiple Files
-----------------------

json\_fdw borrows its semantics from file\_fdw, and associates one foreign table
with one JSON file. If you'd like to query all your JSON files from one table,
you could take one of two approaches. You could either use PostgreSQL's basic
table partitioning feature, and manually create one child table per JSON file.

Alternatively, you could use CitusDB binaries, and "stage" data into a
distributed foreign table. With this approach, you can also have the database
automatically collect statistics about the underlying data, and apply query
optimizations such as partition pruning. For more info, please see our
documentation page at [http://citusdata.com/docs/foreign-data](http://citusdata.com/docs/foreign-data)
, or contact us at engage @ citusdata.com.


Fetching Remote Files
---------------------
The following example shows how to fetch remote files, that are staged locally
for the duration of the query, and then deleted. (Yes, all fetches are presently
treated as an ephemeral operation. Note the TODO at the top)

The existing handling of Gzip files is supported, because, after the file is fetched, it
is handed off to the existing file handling code, as if it were previously staged on disk.

Notes based on how libcurl is built;
1. Both Content Encoding and Transport Encoding are supported.
2. Https should be supported, but has not been tested.

Only curl-7.40.0 has been tested.

    -- create foreign table - using optional get parameters
    CREATE FOREIGN TABLE an_example_table
    (
        fieldName1 TEXT,
        fieldName2 INTEGER,
        . .,
        . .,
        . .
    )
    SERVER json_server
    OPTIONS (filename 'http://www.example.com/file/location/url/some.json.gz?optional=paramaters&separated=traditionally');

    -- create foreign table - using optional post and get parameters
    CREATE FOREIGN TABLE another_example_table
    (
        fieldName1 TEXT,
        fieldName2 INTEGER,
        . .,
        . .,
        . .
    )
    SERVER json_server
    OPTIONS (filename 'http://www.example.com/file/location/url/some.json.gz?optional=paramaters&separated=traditionally', http_post_vars 'another=parameter_set&separated=traditionally&donot=pre-encode_them');


Limitations
-----------

* json\_fdw only supports files that consist of one JSON document per line. It
  doesn't support objects that span multiple lines.

* PostgreSQL limits column names to 63 characters by default. If you need column
  names that are longer, you can increase the NAMEDATALEN constant in
  src/include/pg\_config\_manual.h, compile, and reinstall.


Copyright
---------

Copyright (c) 2013 Citus Data, Inc.

This module is free software; you can redistribute it and/or modify it under the
GNU GPL v3.0 License.

For all types of questions and comments about the wrapper, please contact us at
engage @ citusdata.com.
