# What is pg2arrow?

**pg2arrow** is a simple and lightweight utility to query PostgreSQL relational database, and to dump the query results in Apache Arrow format.

Apache Arrow is a kind of file format that can save massive amount of structured data using columnar layout, thus, optimized to analytic workloads for insert-only dataset.

## Advantages
* Binary data transfer
    * It uses binary transfer mode of `libpq`. It eliminates parse/deparse operations of text encoded query results. Binary operations are fundamentally lightweight, and has less problems to handle cstring literals.
* Exact data type mapping
    * It fetches exact data-type information from PostgreSQL, and keeps data types as is. For example, user defined composite data types are written as `Struct` type (it can have nested child types), however, some implementation encodes these unknown types to string representation anyway.
* Less memory consumption
    * It writes out `RecordBatch` (that is a certain amount of rows) to the result file per specified size (default: 512MB). Since it does not load entire dataset on memory prior to writing out, we can dump billion rows even if it is larger than physical memory.

For more details, see our wikipage: https://github.com/heterodb/pg2arrow/wiki

## Issues

### Building and Dependencies 

Had to modify `Makefile` to fix linker not finding postgres installation.

I found the right path by running `locate pgcommon`

Got errors:
```
/usr/bin/ld: cannot find -lpam
/usr/bin/ld: cannot find -lgssapi_krb5
/usr/bin/ld: cannot find -ledit
```

Ubuntu  `sudo apt-get install libpam0g-dev libedit-dev libkrb5-dev`

Finally, I got it built.

### Generating the file

I'm running this command: `./pg2arrow -d nyc-taxi-data -c 'SELECT id, vendor_id, store_and_fwd_flag, passenger_count FROM trips LIMIT 10;' -h 0.0.0.0 -p 6120 -U postgres -W -o ./limited.arrow`


using these fields from [the NYC taxi schema](https://github.com/toddwschneider/nyc-taxi-data/blob/8e94dabc954c4b5637eda8fda412dfeb6f5a8579/setup_files/create_nyc_taxi_schema.sql#L138-L150):

```sql
id bigserial primary key,
vendor_id text,
store_and_fwd_flag text,
passenger_count integer,
```

Generated file:
```
./pg2arrow --dump ./limited.arrow
[Footer]
{Footer: version=3, schema={Schema: endianness=little, fields=[{Field: name=id, nullable=true, type={Int64}, children=[], custom_metadata=[]}, {Field: name=vendor_id, nullable=true, type={Utf8}, children=[], custom_metadata=[]}, {Field: name=store_and_fwd_flag, nullable=true, type={Utf8}, children=[], custom_metadata=[]}, {Field: name=passenger_count, nullable=true, type={Int32}, children=[], custom_metadata=[]}], custom_metadata []}, dictionaries=[], recordBatches=[{Block: offset=336, metaDataLength=312 bodyLength=448}]}
[Record Batch 0]
{Message: version=3, body={RecordBatch: length=10, nodes=[{FieldNode: length=10, null_count=0}, {FieldNode: length=10, null_count=0}, {FieldNode: length=10, null_count=0}, {FieldNode: length=10, null_count=0}], buffers=[{Buffer: offset=0, length=0}, {Buffer: offset=0, length=128}, {Buffer: offset=128, length=0}, {Buffer: offset=128, length=64}, {Buffer: offset=192, length=64}, {Buffer: offset=256, length=0}, {Buffer: offset=256, length=64}, {Buffer: offset=320, length=64}, {Buffer: offset=384, length=0}, {Buffer: offset=384, length=64}]}, bodyLength=448}
```

But then I have [the issues reading it with other Arrow libraries](https://github.com/heterodb/pg2arrow/issues/14)

For example, the Data Preview extension in VSCode generates the [`limited.schema.json`](./limited.schema.json) file, which shows the correct integer types but an empty object for the string/utf-8 type.