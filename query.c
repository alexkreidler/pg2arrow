/*
 * query.c - interaction to PostgreSQL server
 *
 * Copyright 2018-2019 (C) KaiGai Kohei <kaigai@heterodb.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License. See the LICENSE file.
 */
#include "pg2arrow.h"

#define atooid(x)		((Oid) strtoul((x), NULL, 10))
#define InvalidOid		((Oid) 0)

/* forward declarations */
static SQLtable *
pgsql_create_composite_type(PGconn *conn, Oid comptype_relid);
static SQLattribute *
pgsql_create_array_element(PGconn *conn, Oid array_elemid);

static inline bool
pg_strtobool(const char *v)
{
	if (strcasecmp(v, "t") == 0 ||
		strcasecmp(v, "true") == 0 ||
		strcmp(v, "1") == 0)
		return true;
	else if (strcasecmp(v, "f") == 0 ||
			 strcasecmp(v, "false") == 0 ||
			 strcmp(v, "0") == 0)
		return false;
	Elog("unexpected boolean type literal: %s", v);
}

static inline char
pg_strtochar(const char *v)
{
	if (strlen(v) == 0)
		Elog("unexpected empty string");
	if (strlen(v) > 1)
		Elog("unexpected character string");
	return *v;
}

/*
 * pgsql_setup_attribute
 */
static void
pgsql_setup_attribute(PGconn *conn,
					  SQLattribute *attr,
					  const char *attname,
					  Oid atttypid,
					  int atttypmod,
					  int attlen,
					  char attbyval,
					  char attalign,
					  char typtype,
					  Oid comp_typrelid,
					  Oid array_elemid,
					  const char *nspname,
					  const char *typname,
					  int *p_numFieldNodes,
					  int *p_numBuffers)
{
	attr->attname   = pstrdup(attname);
	attr->atttypid  = atttypid;
	attr->atttypmod = atttypmod;
	attr->attlen    = attlen;
	attr->attbyval  = attbyval;

	if (attalign == 'c')
		attr->attalign = sizeof(char);
	else if (attalign == 's')
		attr->attalign = sizeof(short);
	else if (attalign == 'i')
		attr->attalign = sizeof(int);
	else if (attalign == 'd')
		attr->attalign = sizeof(double);
	else
		Elog("unknown state of attalign: %c", attalign);

	attr->typnamespace = pstrdup(nspname);
	attr->typname = pstrdup(typname);
	attr->typtype = typtype;
	if (typtype == 'b')
	{
		if (array_elemid != InvalidOid)
			attr->elemtype = pgsql_create_array_element(conn, array_elemid);
	}
	else if (typtype == 'c')
	{
		/* composite data type */
		SQLtable   *subtypes;

		assert(comp_typrelid != 0);
		subtypes = pgsql_create_composite_type(conn, comp_typrelid);
		*p_numFieldNodes += subtypes->numFieldNodes;
		*p_numBuffers += subtypes->numBuffers;

		attr->subtypes = subtypes;
	}
#if 0
	else if (typtype == 'd')
	{
		//Domain type has identical definition to the base type
		//expect for its constraint.
	}
	else if (typtype == 'e')
	{
		//Enum type may be ideal for dictionary compression
	}
#endif
	else
		Elog("unknown state pf typtype: %c", typtype);

	/* init statistics */
	attr->min_isnull = true;
	attr->max_isnull = true;
	attr->min_value  = 0UL;
	attr->max_value  = 0UL;
	/* assign properties of Apache Arrow Type */
	assignArrowType(attr, p_numBuffers);
	*p_numFieldNodes += 1;
}

/*
 * pgsql_create_composite_type
 */
static SQLtable *
pgsql_create_composite_type(PGconn *conn, Oid comptype_relid)
{
	PGresult   *res;
	SQLtable   *table;
	char		query[4096];
	int			j, nfields;

	snprintf(query, sizeof(query),
			 "SELECT attname, attnum, atttypid, atttypmod, attlen,"
			 "       attbyval, attalign, typtype, typrelid, typelem,"
			 "       nspname, typname"
			 "  FROM pg_catalog.pg_attribute a,"
			 "       pg_catalog.pg_type t,"
			 "       pg_catalog.pg_namespace n"
			 " WHERE t.typnamespace = n.oid"
			 "   AND a.atttypid = t.oid"
			 "   AND a.attrelid = %u", comptype_relid);
	res = PQexec(conn, query);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		Elog("failed on pg_type system catalog query: %s",
			 PQresultErrorMessage(res));

	nfields = PQntuples(res);
	table = palloc0(offsetof(SQLtable, attrs[nfields]));
	table->nfields = nfields;
	for (j=0; j < nfields; j++)
	{
		const char *attname   = PQgetvalue(res, j, 0);
		const char *attnum    = PQgetvalue(res, j, 1);
		const char *atttypid  = PQgetvalue(res, j, 2);
		const char *atttypmod = PQgetvalue(res, j, 3);
		const char *attlen    = PQgetvalue(res, j, 4);
		const char *attbyval  = PQgetvalue(res, j, 5);
		const char *attalign  = PQgetvalue(res, j, 6);
		const char *typtype   = PQgetvalue(res, j, 7);
		const char *typrelid  = PQgetvalue(res, j, 8);
		const char *typelem   = PQgetvalue(res, j, 9);
		const char *nspname   = PQgetvalue(res, j, 10);
		const char *typname   = PQgetvalue(res, j, 11);
		int			index     = atoi(attnum);

		if (index < 1 || index > nfields)
			Elog("attribute number is out of range");
		pgsql_setup_attribute(conn,
							  &table->attrs[index-1],
							  attname,
							  atooid(atttypid),
							  atoi(atttypmod),
							  atoi(attlen),
							  pg_strtobool(attbyval),
							  pg_strtochar(attalign),
							  pg_strtochar(typtype),
							  atooid(typrelid),
							  atooid(typelem),
							  nspname, typname,
							  &table->numFieldNodes,
							  &table->numBuffers);
	}
	return table;
}

static SQLattribute *
pgsql_create_array_element(PGconn *conn, Oid array_elemid)
{
	SQLattribute   *attr = palloc0(sizeof(SQLattribute));
	PGresult	   *res;
	char			query[4096];
	const char     *nspname;
	const char	   *typname;
	const char	   *typlen;
	const char	   *typbyval;
	const char	   *typalign;
	const char	   *typtype;
	const char	   *typrelid;
	const char	   *typelem;
	int				__dummy__;

	snprintf(query, sizeof(query),
			 "SELECT nspname, typname,"
			 "       typlen, typbyval, typalign, typtype,"
			 "       typrelid, typelem,"
			 "  FROM pg_catalog.pg_type t,"
			 "       pg_catalog.pg_namespace n"
			 " WHERE t.typnamespace = n.oid"
			 "   AND t.oid = %u", array_elemid);
	res = PQexec(conn, query);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		Elog("failed on pg_type system catalog query: %s",
			 PQresultErrorMessage(res));
	if (PQntuples(res) != 1)
		Elog("unexpected number of result rows: %d", PQntuples(res));
	nspname  = PQgetvalue(res, 0, 0);
	typname  = PQgetvalue(res, 0, 1);
	typlen   = PQgetvalue(res, 0, 2);
	typbyval = PQgetvalue(res, 0, 3);
	typalign = PQgetvalue(res, 0, 4);
	typtype  = PQgetvalue(res, 0, 5);
	typrelid = PQgetvalue(res, 0, 6);
	typelem  = PQgetvalue(res, 0, 7);

	pgsql_setup_attribute(conn,
						  attr,
						  typname,
						  array_elemid,
						  -1,
						  atoi(typlen),
						  pg_strtobool(typbyval),
						  pg_strtochar(typalign),
						  pg_strtochar(typtype),
						  atooid(typrelid),
						  atooid(typelem),
						  nspname,
						  typname,
						  &__dummy__,
						  &__dummy__);
	return attr;
}

/*
 * pgsql_create_buffer
 */
SQLtable *
pgsql_create_buffer(PGconn *conn, PGresult *res, size_t segment_sz)
{
	int			j, nfields = PQnfields(res);
	SQLtable   *table;

	table = palloc0(offsetof(SQLtable, attrs[nfields]));
	table->segment_sz = segment_sz;
	table->nitems = 0;
	table->nfields = nfields;
	for (j=0; j < nfields; j++)
	{
		const char *attname = PQfname(res, j);
		Oid			atttypid = PQftype(res, j);
		int			atttypmod = PQfmod(res, j);
		PGresult   *__res;
		char		query[4096];
		const char *typlen;
		const char *typbyval;
		const char *typalign;
		const char *typtype;
		const char *typrelid;
		const char *typelem;
		const char *nspname;
		const char *typname;

		snprintf(query, sizeof(query),
				 "SELECT typlen, typbyval, typalign, typtype,"
				 "       typrelid, typelem, nspname, typname"
				 "  FROM pg_catalog.pg_type t,"
				 "       pg_catalog.pg_namespace n"
				 " WHERE t.typnamespace = n.oid"
				 "   AND t.oid = %u", atttypid);
		__res = PQexec(conn, query);
		if (PQresultStatus(__res) != PGRES_TUPLES_OK)
			Elog("failed on pg_type system catalog query: %s",
				 PQresultErrorMessage(res));
		if (PQntuples(__res) != 1)
			Elog("unexpected number of result rows: %d", PQntuples(__res));
		typlen   = PQgetvalue(__res, 0, 0);
		typbyval = PQgetvalue(__res, 0, 1);
		typalign = PQgetvalue(__res, 0, 2);
		typtype  = PQgetvalue(__res, 0, 3);
		typrelid = PQgetvalue(__res, 0, 4);
		typelem  = PQgetvalue(__res, 0, 5);
		nspname  = PQgetvalue(__res, 0, 6);
		typname  = PQgetvalue(__res, 0, 7);
		pgsql_setup_attribute(conn,
							  &table->attrs[j],
                              attname,
							  atttypid,
							  atttypmod,
							  atoi(typlen),
							  *typbyval,
							  *typalign,
							  *typtype,
							  atoi(typrelid),
							  atoi(typelem),
							  nspname, typname,
							  &table->numFieldNodes,
							  &table->numBuffers);
		PQclear(__res);
	}
	return table;
}

/*
 * pgsql_clear_attribute
 */
static void
pgsql_clear_attribute(SQLattribute *attr)
{
	attr->nullcount = 0;
	sql_buffer_clear(&attr->nullmap);
	sql_buffer_clear(&attr->values);
	sql_buffer_clear(&attr->extra);

	if (attr->subtypes)
	{
		SQLtable   *subtypes = attr->subtypes;
		int			j;

		for (j=0; j < subtypes->nfields; j++)
			pgsql_clear_attribute(&subtypes->attrs[j]);
	}
	if (attr->elemtype)
		pgsql_clear_attribute(attr->elemtype);
	/* clear statistics */
	attr->min_isnull = true;
	attr->max_isnull = true;
	attr->min_value  = 0UL;
	attr->max_value  = 0UL;
}

/*
 * pgsql_writeout_buffer
 */
void
pgsql_writeout_buffer(SQLtable *table)
{
	off_t		currPos;
	size_t		metaSize;
	size_t		bodySize;
	int			j, index;
	ArrowBlock *b;

	/* write a new record batch */
	currPos = lseek(table->fdesc, 0, SEEK_CUR);
	if (currPos < 0)
		Elog("unable to get current position of the file");
	writeArrowRecordBatch(table, &metaSize, &bodySize);

	index = table->numRecordBatches++;
	if (index == 0)
		table->recordBatches = palloc(sizeof(ArrowBlock));
	else
		table->recordBatches = repalloc(table->recordBatches,
										sizeof(ArrowBlock) * (index+1));
	b = &table->recordBatches[index];
	b->tag = ArrowNodeTag__Block;
	b->offset = currPos;
	b->metaDataLength = metaSize;
	b->bodyLength = bodySize;

	/* makes table/attributes empty */
	table->nitems = 0;
	for (j=0; j < table->nfields; j++)
		pgsql_clear_attribute(&table->attrs[j]);
}

/*
 * pgsql_append_results
 */
void
pgsql_append_results(SQLtable *table, PGresult *res)
{
	int		i, ntuples = PQntuples(res);
	int		j, nfields = PQnfields(res);
	size_t	usage;

	assert(nfields == table->nfields);
	for (i=0; i < ntuples; i++)
	{
	retry:
		usage = 0;
		for (j=0; j < nfields; j++)
		{
			SQLattribute   *attr = &table->attrs[j];
			const char	   *addr;
			size_t			sz;
			/* data must be binary format */
			assert(PQfformat(res, j) == 1);
			if (PQgetisnull(res, i, j))
			{
				addr = NULL;
				sz = 0;
			}
			else
			{
				addr = PQgetvalue(res, i, j);
				sz = PQgetlength(res, i, j);
			}
			usage += attr->put_value(attr, table->nitems, addr, sz);
		}
		/* check threshold to write out */
		if (usage > table->segment_sz)
		{
			if (table->nitems == 0)
				Elog("A result row is larger than size of record batch!!");
			/* fixup NULL-count if last row updated it */
			for (j=0; j < nfields; j++)
			{
				SQLattribute   *attr = &table->attrs[j];

				if (PQgetisnull(res, i, j))
				{
					assert(attr->nullcount > 0);
					attr->nullcount--;
				}
			}
			/* write out the bunch of query results */
			pgsql_writeout_buffer(table);
			goto retry;
		}
		/* update statistics */
		for (j=0; j < nfields; j++)
		{
			SQLattribute   *attr = &table->attrs[j];
			const char	   *addr;
			size_t			sz;

			if (!attr->stat_update)
				continue;
			if (PQgetisnull(res, i, j))
			{
				addr = NULL;
				sz = 0;
			}
			else
			{
				addr = PQgetvalue(res, i, j);
				sz = PQgetlength(res, i, j);
			}
			attr->stat_update(attr, addr, sz);
		}
		table->nitems++;
	}
}

/*
 * pgsql_dump_attribute
 */
static void
pgsql_dump_attribute(SQLattribute *attr, const char *label, int indent)
{
	int		j;

	for (j=0; j < indent; j++)
		putchar(' ');
	printf("%s {attname='%s', atttypid=%u, atttypmod=%d, attlen=%d,"
		   " attbyval=%s, attalign=%d, typtype=%c, arrow_type=",
		   label,
		   attr->attname, attr->atttypid, attr->atttypmod, attr->attlen,
		   attr->attbyval ? "true" : "false", attr->attalign, attr->typtype);
	dumpArrowNode((ArrowNode *)&attr->arrow_type, stdout);
	printf("}\n");

	if (attr->typtype == 'b')
	{
		if (attr->elemtype)
			pgsql_dump_attribute(attr->elemtype, "element", indent+2);
	}
	else if (attr->typtype == 'c')
	{
		SQLtable   *subtypes = attr->subtypes;
		char		label[64];

		for (j=0; j < subtypes->nfields; j++)
		{
			snprintf(label, sizeof(label), "subtype[%d]", j);
			pgsql_dump_attribute(&subtypes->attrs[j], label, indent+2);
		}
	}
}

/*
 * pgsql_dump_buffer
 */
void
pgsql_dump_buffer(SQLtable *table)
{
	int		j;
	char	label[64];

	printf("Dump of SQL buffer:\n"
		   "nfields: %d\n"
		   "nitems: %zu\n",
		   table->nfields,
		   table->nitems);
	for (j=0; j < table->nfields; j++)
	{
		snprintf(label, sizeof(label), "attr[%d]", j);
		pgsql_dump_attribute(&table->attrs[j], label, 0);
	}
}
