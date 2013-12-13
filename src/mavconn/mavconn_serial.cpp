/**
 * @file mavconn_serial.cpp
 * @author Vladimit Ermkov <voon341@gmail.com>
 */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "mavconn_serial.h"
#include <ros/console.h>
#include <ros/assert.h>

using namespace mavconn;

MAVConnSerial::MAVConnSerial(uint8_t system_id, uint8_t component_id,
		std::string device, unsigned baudrate) :
	io_service(),
	serial_dev(io_service, device)
{
	sys_id = system_id;
	comp_id = component_id;
	channel = new_channel();
	ROS_ASSERT_MSG(channel >= 0, "channel allocation failure");

	serial_dev.set_option(boost::asio::serial_port_base::baud_rate(baudrate));
	serial_dev.set_option(boost::asio::serial_port_base::character_size(8));
	serial_dev.set_option(boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none));
	serial_dev.set_option(boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one));
	serial_dev.set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none));

	// give some work to io_service before start
	io_service.post(boost::bind(&MAVConnSerial::do_read, this));

	// run io_service for async io
	boost::thread t(boost::bind(&boost::asio::io_service::run, &this->io_service));
	io_thread.swap(t);
}

MAVConnSerial::~MAVConnSerial()
{
	io_service.stop();
}

bool MAVConnSerial::send_message(const mavlink_message_t *message, uint8_t sysid, uint8_t compid)
{
	ROS_ASSERT(message != NULL);
	uint8_t buffer[MAVLINK_MAX_PACKET_LEN + 2];
	mavlink_message_t msg = *message;

	mavlink_finalize_message_chan(&msg, sysid, compid, channel, message->len, mavlink_crcs[msg.msgid]);
	size_t length = mavlink_msg_to_send_buffer(buffer, &msg);

	ROS_DEBUG_NAMED("mavconn", "serial::send_message: Message-ID: %d [%zu bytes]", message->msgid, length);

	{
		boost::recursive_mutex::scoped_lock lock(mutex);
		tx_q.insert(tx_q.end(), buffer, buffer + length);
	}
	io_service.post(boost::bind(&MAVConnSerial::do_write, this));
}

void MAVConnSerial::do_read(void)
{
	serial_dev.async_read_some(
			boost::asio::buffer(rx_buf, sizeof(rx_buf)),
			boost::bind(&MAVConnSerial::async_read_end,
				this,
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred));
}

void MAVConnSerial::async_read_end(boost::system::error_code error, size_t bytes_transfered)
{
	if (error) {
		if (serial_dev.is_open()) {
			serial_dev.close();
			port_closed();
			ROS_ERROR_NAMED("mavconn", "serial::async_read_end: error! port closed.");
		}
	} else {
		mavlink_message_t message;
		mavlink_status_t status;

		for (size_t i = 0; i < bytes_transfered; i++) {
			if (mavlink_parse_char(channel, rx_buf[i], &message, &status)) {
				ROS_DEBUG_NAMED("mavconn", "serial::async_read_end: recv Message-Id: %d Sys-Id: %d Comp-Id: %d",
						message.msgid, message.sysid, message.compid);

				/* emit */ message_received(&message, message.sysid, message.compid);
			}
		}

		do_read();
	}
}

void MAVConnSerial::do_write(void)
{
	// if write not in progress
	if (tx_buf == 0) {
		boost::recursive_mutex::scoped_lock lock(mutex);

		tx_buf_size = tx_q.size();
		tx_buf.reset(new uint8_t[tx_buf_size]);
		std::copy(tx_q.begin(), tx_q.end(), tx_buf.get());
		tx_q.clear();

		boost::asio::async_write(serial_dev,
				boost::asio::buffer(tx_buf.get(), tx_buf_size),
				boost::bind(&MAVConnSerial::async_write_end,
					this,
					boost::asio::placeholders::error));
	}
}

void MAVConnSerial::async_write_end(boost::system::error_code error)
{
	if (!error) {
		boost::recursive_mutex::scoped_lock lock(mutex);

		if (tx_q.empty()) {
			tx_buf.reset();
			tx_buf_size = 0;
			return;
		}

		tx_buf_size = tx_q.size();
		tx_buf.reset(new uint8_t[tx_buf_size]);
		std::copy(tx_q.begin(), tx_q.end(), tx_buf.get());
		tx_q.clear();

		boost::asio::async_write(serial_dev,
				boost::asio::buffer(tx_buf.get(), tx_buf_size),
				boost::bind(&MAVConnSerial::async_write_end,
					this,
					boost::asio::placeholders::error));
	} else {
		if (serial_dev.is_open()) {
			serial_dev.close();
			port_closed();
			ROS_ERROR_NAMED("mavconn", "serial::async_write_end: error! port closed.");
		}
	}
}

