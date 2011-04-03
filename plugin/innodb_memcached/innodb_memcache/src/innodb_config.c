#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "innodb_api.h"
#include <errno.h>

#include "innodb_config.h"

/**********************************************************************//**
Makes a NUL-terminated copy of a nonterminated string.
@return own: a copy of the string, must be deallocated with mem_free */
static
char*
my_strdupl(
/*=======*/
        const char*     str,    /*!< in: string to be copied */
        ulint           len)    /*!< in: length of str, in bytes */
{
        char*   s = (char*) malloc(len + 1);
        s[len] = 0;
        return((char*) memcpy(s, str, len));
}

/**********************************************************************//**
This function frees meta info structure */
void
innodb_config_free(
/*===============*/
	meta_info_t*	item)	/*!< in: meta info structure */
{
	int	i;

	for (i = 0; i < META_CONTAINER_TO_GET; i++) {
		if (item->m_item[i].m_str) {
			free(item->m_item[i].m_str);
		}
	}

	if (item->m_index.m_name) {
		free(item->m_index.m_name);
	}

	if (item->m_add_item) {
		for (i = 0; i < item->m_num_add; i++) {
			free(item->m_add_item[i].m_str);
		}

		free(item->m_add_item);
	}

	return;
}

/**********************************************************************//**
This function parses possible multiple column name separated by ",", ";"
or " " in the input "str" for the memcached "value" field.
@return TRUE if everything works out fine */
static
bool
innodb_config_parse_value_col(
/*==========================*/
	meta_info_t*	item,		/*!< in: meta info structure */
	char*		str,		/*!< in: column name(s) string */
	int		len)		/*!< in: length of above string */
{
        static const char*	sep = " ;,";
        char*			last;
	char*			column_str;
	int			num_cols = 0;
	char*			my_str = my_strdupl(str, len);

	assert(my_str);

	/* Find out how many column names in the string */
	for (column_str = strtok_r(my_str, sep, &last);
	     column_str;
	     column_str = strtok_r(NULL, sep, &last)) {
		num_cols++;
	}

	free(my_str);

	item->m_num_add = num_cols;

	my_str = str;

	if (num_cols > 1) {
		int	i = 0;
		item->m_add_item = malloc(num_cols * sizeof(*item->m_add_item));

		for (column_str = strtok_r(my_str, sep, &last);
		     column_str;
		     column_str = strtok_r(NULL, sep, &last)) {
			item->m_add_item[i].m_len = strlen(column_str);
			item->m_add_item[i].m_str = my_strdupl(
				column_str, item->m_add_item[i].m_len);
			i++;
		}

		assert(i == num_cols);
	} else {
		item->m_add_item = NULL;
	}

	return(TRUE);
}

/**********************************************************************//**
This function opens the default configuration table, and find the
table and column info that used for memcached data
@return TRUE if everything works out fine */
bool
innodb_config(
/*==========*/
	meta_info_t*	item)	/*!< in: meta info structure */
{
	ib_trx_t		ib_trx;
	ib_crsr_t		crsr = NULL;
	ib_crsr_t		idx_crsr = NULL;
	ib_tpl_t		tpl = NULL;
	ib_err_t		err = DB_SUCCESS;
	int			n_cols;
	int			i;
	ib_ulint_t		data_len;
	ib_col_meta_t		col_meta;

	memset(item, 0, sizeof(*item));

	ib_trx = ib_cb_trx_begin(IB_TRX_READ_COMMITTED);
	err = innodb_api_begin(NULL, INNODB_META_DB,
			       INNODB_META_CONTAINER_TABLE, ib_trx,
			       &crsr, &idx_crsr, IB_LOCK_IS);	

	if (err != DB_SUCCESS) {
		fprintf(stderr, "  InnoDB_Memcached: Please create config table"
				"'%s' in database '%s' by running"
				" 'scripts/innodb_config.sql'\n",
			INNODB_META_CONTAINER_TABLE, INNODB_META_DB);
		err = DB_ERROR;
		goto func_exit;
	}

	tpl = ib_clust_read_tuple_create(crsr);

	/* Currently, we support one table per memcached set up.
	We could extend that later */
	err = ib_cb_cursor_first(crsr);

	if (err != DB_SUCCESS) {
		fprintf(stderr, "  InnoDB_Memcached: fail to locate entry in"
				" config table '%s' in database '%s' \n",
			INNODB_META_CONTAINER_TABLE, INNODB_META_DB);
		err = DB_ERROR;
		goto func_exit;
	}

	err = ib_cb_read_row(crsr, tpl);

	n_cols = ib_cb_tuple_get_n_cols(tpl);

	assert(n_cols >= META_CONTAINER_TO_GET);

	for (i = 0; i < META_CONTAINER_TO_GET; ++i) {

		data_len = ib_cb_col_get_meta(tpl, i, &col_meta);

		assert(data_len != IB_SQL_NULL);

		item->m_item[i].m_len = data_len;

		item->m_item[i].m_str = my_strdupl(
			(char*)ib_cb_col_get_value(tpl, i), data_len);

		if (i == META_VALUE) {
			innodb_config_parse_value_col(
				item, item->m_item[i].m_str, data_len);
		}
	}

	/* Last column is about the unique index name on key column */
	data_len = ib_cb_col_get_meta(tpl, i, &col_meta);

	if (data_len == IB_SQL_NULL) {
		fprintf(stderr, "  InnoDB_Memcached: There must be a unique"
				" index on memcached table's key column \n");
		err = DB_ERROR;
		goto func_exit;
	}

	item->m_index.m_name = my_strdupl((char*)ib_cb_col_get_value(tpl, i),
					  data_len);

func_exit:

	if (crsr) {
		ib_cb_cursor_close(crsr);
	}

	if (tpl) {
		ib_cb_tuple_delete(tpl);
	}

	ib_cb_trx_commit(ib_trx);

	return(err == DB_SUCCESS);
}

/**********************************************************************//**
This function parses possible multiple column name separated by ",", ";"
or " " in the input "str" for the memcached "value" field
@return TRUE if everything works out fine */
static
ib_err_t
innodb_config_value_col_verify(
/*===========================*/
	char*		name,		/*!< in: column name */
	meta_info_t*	meta_info,	/*!< in: meta info structure */
	ib_col_meta_t*	col_meta,	/*!< in: column metadata */
	int		col_id)		/*!< in: column ID */
{
	ib_err_t	err = DB_NOT_FOUND;

	if (!meta_info->m_add_item) {
		meta_column_t*	cinfo = meta_info->m_item;

		if (strcmp(name, cinfo[META_VALUE].m_str) == 0) {
			if (col_meta->type != IB_VARCHAR
			    && col_meta->type != IB_CHAR
			    && col_meta->type != IB_BLOB) {
				err = DB_DATA_MISMATCH;
			}

			cinfo[META_VALUE].m_field_id = col_id;
			cinfo[META_VALUE].m_col = *col_meta;
			err = DB_SUCCESS;
		}
	} else {
		int	i;

		for (i = 0; i < meta_info->m_num_add; i++) {
			if (strcmp(name, meta_info->m_add_item[i].m_str) == 0) {
				if (col_meta->type != IB_VARCHAR
				    && col_meta->type != IB_CHAR
				    && col_meta->type != IB_BLOB) {
					err = DB_DATA_MISMATCH;
				}

				meta_info->m_add_item[i].m_field_id = col_id;
				meta_info->m_add_item[i].m_col = *col_meta;

				/* FIXME: this is how we going to parse
				the value to be stored in multiple column. */
				meta_info->m_item[META_VALUE].m_field_id = col_id;
				meta_info->m_item[META_VALUE].m_col = *col_meta;
				err = DB_SUCCESS;
			}
		}

	}

	return(err);
}
				
			
/**********************************************************************//**
This function verify the table configuration information, and fill
in columns used for memcached functionalities (cas, exp etc.)
@return TRUE if everything works out fine */
bool
innodb_verify(
/*==========*/
	meta_info_t*       container)	/*!< in: meta info structure */
{
	ib_crsr_t	crsr = NULL;
	ib_crsr_t	idx_crsr = NULL;
	ib_tpl_t	tpl = NULL;
	ib_col_meta_t	col_meta;
	char            table_name[MAX_TABLE_NAME_LEN + MAX_DATABASE_NAME_LEN];
	char*		dbname;
	char*		name;
	int		n_cols;
	int		i;
	ib_err_t	err = DB_SUCCESS;
	bool		is_key_col = FALSE;
	bool		is_value_col = FALSE;
	int		index_type;
	ib_id_t		index_id;

	dbname = container->m_item[META_DB].m_str;
	name = container->m_item[META_TABLE].m_str;
	container->flag_enabled = FALSE;
	container->cas_enabled = FALSE;
	container->exp_enabled = FALSE;

#ifdef __WIN__
	sprintf(table_name, "%s\%s", dbname, name);
#else
	snprintf(table_name, sizeof(table_name), "%s/%s", dbname, name);
#endif

	err = ib_cb_open_table(table_name, NULL, &crsr);

	if (err != DB_SUCCESS) {
		fprintf(stderr, "  InnoDB_Memcached: fail to open table"
				" '%s' \n", table_name);
		err = DB_ERROR;
		goto func_exit;
	}

	tpl = ib_clust_read_tuple_create(crsr);

	n_cols = ib_tuple_get_n_cols(tpl);

	for (i = 0; i < n_cols; i++) {
		ib_err_t	result = DB_SUCCESS;
		meta_column_t*	cinfo = container->m_item;

		name = ib_cb_col_get_name(crsr, i);
		ib_cb_col_get_meta(tpl, i, &col_meta);

		result = innodb_config_value_col_verify(
			name, container, &col_meta, i);

		if (result == DB_SUCCESS) {
			is_value_col = TRUE;
			continue;
		} else if (result == DB_DATA_MISMATCH) {
			err = DB_DATA_MISMATCH;
			goto func_exit;
		}

		if (strcmp(name, cinfo[META_KEY].m_str) == 0) {
			if (col_meta.type != IB_VARCHAR
			    && col_meta.type != IB_CHAR) {
				err = DB_DATA_MISMATCH;
				goto func_exit;
			}
			cinfo[META_KEY].m_field_id = i;
			cinfo[META_KEY].m_col = col_meta;
			is_key_col = TRUE;
		} else if (strcmp(name, cinfo[META_FLAG].m_str) == 0) {
			if (col_meta.type != IB_INT) {
				err = DB_DATA_MISMATCH;
				goto func_exit;
			}
			cinfo[META_FLAG].m_field_id = i;
			cinfo[META_FLAG].m_col = col_meta;
			container->flag_enabled = TRUE;
		} else if (strcmp(name, cinfo[META_CAS].m_str) == 0) {
			if (col_meta.type != IB_INT) {
				err = DB_DATA_MISMATCH;
				goto func_exit;
			}
			cinfo[META_CAS].m_field_id = i;
			cinfo[META_CAS].m_col = col_meta;
			container->cas_enabled = TRUE;
		} else if (strcmp(name, cinfo[META_EXP].m_str) == 0) {
			if (col_meta.type != IB_INT) {
				err = DB_DATA_MISMATCH;
				goto func_exit;
			}
			cinfo[META_EXP].m_field_id = i;
			cinfo[META_EXP].m_col = col_meta;
			container->exp_enabled = TRUE;
		}
	}

	/* Key column and Value column must present */
	if (!is_key_col || !is_value_col) {
		fprintf(stderr, "  InnoDB_Memcached: fail to locate key"
				" column or value column in table"
				" '%s' as specified by config table \n",
			table_name);

		err = DB_ERROR;
		goto func_exit;
	}

	ib_cb_cursor_open_index_using_name(crsr, container->m_index.m_name,
					   &idx_crsr, &index_type, &index_id);

	if (index_type & IB_CLUSTERED) {
		assert(!idx_crsr);
		container->m_index.m_use_idx = META_CLUSTER;
	} else if (!idx_crsr || !(index_type & IB_UNIQUE)) {
		fprintf(stderr, "  InnoDB_Memcached: Index on key column"
				" must be a Unique index\n");
		container->m_index.m_use_idx = META_NO_INDEX;
		err = DB_ERROR;
	} else {
		container->m_index.m_id = index_id;
		container->m_index.m_use_idx = META_SECONDARY;
		container->m_index.m_idx_crsr = idx_crsr;
	}

	if (idx_crsr) {
		ib_cb_cursor_close(idx_crsr);
	}
func_exit:

	if (tpl) {
		ib_cb_tuple_delete(tpl);
	}

	if (crsr) {
		ib_cb_cursor_close(crsr);
	}

	return(err == DB_SUCCESS);
}
