You are a software engineer working on MariaDB.

I run a platform for hosting MariaDB schemas. I run medium-sized 32-128 GB database servers and host multiple (100 to 1000) schemas of different sizes (10MB - 10GB) on each server. I want to keep track of buffer pool memory per schema, so I can move noisy neighbours away to a different server.

innodb_buffer_stats_by_schema does just that, but the query takes too long ~10 minutes. I want something that can do it in ~10 seconds. I should be easily able to scan ~128 GB of memory in ~10 seconds.

Refer to the following files
- scripts/sys_schema/views/i_s/innodb_buffer_stats_by_schema.sql
- i_s.cc implements INNODB_BUFFER_PAGE

The i_s.cc file is large; look for the following 
- i_s_innodb_buffer_page
- i_s_innodb_buffer_page_init 
- i_s_innodb_buffer_page_fill

Explain why the query is taking so long. Suggest a way to solve this problem.

Don't change any of the existing implementation. Create an alternate INNODB_BUFFER_PAGE_STATS_BY_SCHEMA table that will implement the solution. So that I can get the buffer page usage per schema as fast as possible. Create a variable to guard code to populate the table.

Use a Docker container to build MariaDB. Refer to mariadb-build.md. Use a remote Docker host for faster builds root@68.183.80.221. I've already configured SSH access.

I am the only user for this. I can build and deploy my own MariaDB binaries

- Keep the code in a separate file for readability, ease of review, and managing diffs across multiple version updates
- Keep the changes as small as possible
- Keep methods small, ~10 lines
- Write tests

Prepare context about the problem and write a detailed specification in a file. Keep it simple and elegant.
