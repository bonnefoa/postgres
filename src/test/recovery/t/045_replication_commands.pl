# Copyright (c) 2025, PostgreSQL Global Development Group

# Test of replication commands
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 'logical');
$node_primary->start;

my $primary_host = $node_primary->host;
my $primary_port = $node_primary->port;
my $connstr_db = "host=$primary_host port=$primary_port replication=database dbname=postgres";

my ($ret, $stdout, $stderr) = $node_primary->psql(
	'postgres', qq[
		CREATE_REPLICATION_SLOT "test_slot" LOGICAL "test_decoding" ( SNAPSHOT "export");
		DROP_REPLICATION_SLOT "test_slot";
	],
	on_error_die => 1,
	extra_params => [ '-d', $connstr_db ]);
ok($ret == 0, "Create and drop of replication slot");

# Testcase end
# =============================================================================

done_testing();

