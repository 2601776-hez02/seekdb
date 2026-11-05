--disable_query_log
let $para_value = query_get_value(select value from parameter_stat() where name = '$para_name', value, 1);
eval alter system set $para_name = '$para_value' ;
--enable_query_log
