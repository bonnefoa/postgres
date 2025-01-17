
# Copyright (c) 2025, PostgreSQL Global Development Group

=pod

=head1 NAME

PostgreSQL::Test::PgProto - class for manipulating PG protocol over a raw socket

=head1 SYNOPSIS

  use PostgreSQL::Test::PgProto;

  my $node = PostgreSQL::Test::PgProto->new('mynode');

  # Create a data directory with initdb
  $node->init();

  # Start the PostgreSQL server
  $node->start();

  # Get raw socket to the node
  my $sock = $node->raw_connect();

  # Create pgproto from the raw socket
  my $pgproto = PostgreSQL::Test::PgProto->new($sock);

  # Send startup packet
  my %parameters = ( user => "postgres", database => "postgres", application_name => "app" );
  $pgproto->send_startup_message(\%parameters);

  # Read startup sequence until Ready For Query is reached
  $pgproto->read_until_message('Z');

  # Send a simple query
  $pgproto->send_simple_query('SELECT 1');

=head1 DESCRIPTION

PostgreSQL::Test::PgProto contains functionality for sending and reading
PostgreSQL protocol messages over a raw socket.

=cut

package PostgreSQL::Test::PgProto;
use Test::More;

use strict;
use warnings FATAL => 'all';

=pod

=head1 METHODS

=over

=item PostgreSQL::Test::PgProto->new($sock)

Builds a new object of class C<PostgreSQL::Test::PgProto>.

=cut

sub new
{
	my $class = shift;
	my ($sock) = @_;

	my $pgproto = {
		'sock' => $sock
	};
	bless $pgproto, $class;
	return $pgproto;
}

=pod

=item $pgproto->send_startup_message

Send a startup message with the provided parameters. Database parameters need to
be passed as a reference to a hash. The server's response is not consumed.

=cut

sub send_startup_message
{
	my $self = shift;
	my $parameters = shift;

	# Startup packet contains:
	# Packet length: 4 bytes
	# Major proto version: 2 bytes
	# Minor proto version: 2 bytes
	# Multiple Parameters:
	#     Parameter Name (null terminated string)
	#     Parameter Value (null terminated string)
	# Ending null character
	my $pack_template = "Nnn(Z*Z*)" . keys(%{$parameters}) . 'x';
	# Packet length, proto and final null character
	my $total_length = 9;

	for(keys %{$parameters}){
		my $key_length = length($_) + 1;
		my $value_length = length($parameters->{$_}) + 1;
		$total_length += $key_length + $value_length;
	}

	my $startup_packet = pack($pack_template, $total_length, 3, 0, %{$parameters});
	$self->{sock}->send($startup_packet);
}

=pod

=item $pgproto->send_ssl_request

Send an ssl request packet to the server. The server's response is not consumed.

=cut

sub send_ssl_request
{
	my $self = shift;

	# SSLRequest packet contains:
	# Packet length: 4 bytes
	# NEGOTIATE_SSL_CODE (1234, 5679): 4 bytes
	my $ssl_request_packet = pack("Nnn", 8, 1234, 5679);

	$self->{sock}->send($ssl_request_packet);
}

=pod

=item $pgproto->send_simple_query

Send a simple query message to the server. The response is not consumed.

=cut

sub send_simple_query
{
	my ($self, $query) = @_;

	# Query message contains:
	# Message type 'Q' (1 byte)
	# Message length not including message type (4 bytes)
	# Null terminated string
	my $query_packet = pack("CNZ*", ord('Q'), length($query) + 5, $query);
	note "Sending following simple query through raw_tcp: $query";
	$self->{sock}->send($query_packet);
}

=pod

=item $pgproto->read_session_pid

Returns the pid of the session. The session needs to be in a ready for query
state. All results will be consumed and will leave the session in a ready for
query state.

=cut

sub read_session_pid
{
	my ($self) = @_;

	$self->send_simple_query("select pg_backend_pid()");
	my $data_row = $self->read_until_message('D');
	# We should have only one field and one column with the
	# pid representing the rest of the payload outside of the
	# data row header
	my ($field_count, $column_length, $pid) = unpack("nNA*", $data_row);
	note "raw_tcp has pid $pid";
	# Consume until Ready for query is reached
	$self->read_until_message('Z');
	return $pid;
}

=pod

=item $pgproto->wait_until_closed

Block and read all responses until the socket is terminated by the server.

=cut

sub wait_until_closed
{
	my ($self) = @_;
	my $received = "";

	while (1) {
		$self->{sock}->recv($received, 64*1024);
		if ($received eq "") {
			# Closed socket was detected
			return;
		}
	}
}

=pod

=item $pgproto->read_type

Read a 1 byte type from the socket.

=cut

sub read_type
{
	my ($self) = @_;

	my $type = "";
	$self->{sock}->recv($type, 1);
	return $type;
}

=pod

=item $pgproto->read_length

Read a 4 bytes length from the socket.

=cut

sub read_length
{
	my ($self) = @_;

	my $length_reply = "";
	$self->{sock}->recv($length_reply, 4);
	my ($length) = unpack("N", $length_reply);
	return $length;
}

=pod

=item $pgproto->read_until_message

Read all messages from the server until C<message_type> is found. The message's
payload will be returned.

=cut

sub read_until_message
{
	my ($self, $message_type) = @_;

	note "Reading until message of type $message_type is found";
	while (1)
	{
		my $type = $self->read_type();
		if ($type eq "") {
			diag("Reached end of the socket before type $message_type was found");
			return;
		}
		my $length = $self->read_length();
		note "Reading message of type $type and length $length";

		# Need to remove message's length from the payload's length
		$length -= 4;
		if ($length < 0) {
			diag("read_until_message Unexpected payload length $length");
			return;
		}
		my $payload = "";
		$self->{sock}->recv($payload, $length);

		if ($type eq $message_type) {
			# We've found the desired message type
			note "Found expected message type $message_type";
			return $payload;
		}
	}
}

1;
