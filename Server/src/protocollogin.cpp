/**
 * Tibia GIMUD Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Sabrehaven and Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "otpch.h"

#include "protocollogin.h"

#include "outputmessage.h"
#include "tasks.h"

#include "configmanager.h"
#include "iologindata.h"
#include "ban.h"
#include "game.h"

extern ConfigManager g_config;
extern Game g_game;

void ProtocolLogin::disconnectClient(const std::string& message, uint16_t version)
{
	auto output = OutputMessagePool::getOutputMessage();

	output->addByte(version >= 1076 ? 0x0B : 0x0A);
	output->addString(message);
	send(output);

	disconnect();
}

void ProtocolLogin::getCharacterList(uint32_t accountNumber, const std::string& password, uint16_t version)
{
	Account account;
	if (!IOLoginData::loginserverAuthentication(accountNumber, password, account)) {
		disconnectClient("Accountnumber or password is not correct.", version);
		return;
	}

	auto output = OutputMessagePool::getOutputMessage();

	//Update premium days
	Game::updatePremium(account);

	const std::string& motd = g_config.getString(ConfigManager::MOTD);
	if (!motd.empty()) {
		//Add MOTD
		output->addByte(0x14);

		std::ostringstream ss;
		ss << g_game.getMotdNum() << "\n" << motd;
		output->addString(ss.str());
	}

	//Add char list
	output->addByte(0x64);

	uint8_t size = std::min<size_t>(std::numeric_limits<uint8_t>::max(), account.characters.size());
	output->addByte(size);
	for (uint8_t i = 0; i < size; i++) {
		output->addString(account.characters[i]);
		output->addString(g_config.getString(ConfigManager::SERVER_NAME));
		output->add<uint32_t>(inet_addr(g_config.getString(ConfigManager::IP).c_str()));
		output->add<uint16_t>(g_config.getNumber(ConfigManager::GAME_PORT));
	}

	//Add premium days
	if (g_config.getBoolean(ConfigManager::FREE_PREMIUM)) {
		output->add<uint16_t>(0xFFFF);
	}
	else {
		output->add<uint16_t>(account.premiumDays);
	}

	send(output);

	disconnect();
}


void ProtocolLogin::onRecvFirstMessage(NetworkMessage& msg)
{
	if (g_game.getGameState() == GAME_STATE_SHUTDOWN) {
		disconnect();
		return;
	}

	msg.skipBytes(2); // client OS

	uint16_t version = msg.get<uint16_t>();
	if (version >= 971) {
		msg.skipBytes(17);
	}
	else {
		msg.skipBytes(12);
	}

	/*
	 * Skipped bytes:
	 * 4 bytes: protocolVersion
	 * 12 bytes: dat, spr, pic signatures (4 bytes each)
	 * 1 byte: 0
	 */

	if (version <= 760) {
		std::ostringstream ss;
		ss << "Only clients with protocol " << getClientVersionString(g_game.getClientVersion()) << " allowed!";
		disconnectClient(ss.str(), version);
		return;
	}

	if (!Protocol::RSA_decrypt(msg)) {
		disconnect();
		return;
	}

	uint32_t key[4];
	key[0] = msg.get<uint32_t>();
	key[1] = msg.get<uint32_t>();
	key[2] = msg.get<uint32_t>();
	key[3] = msg.get<uint32_t>();
	enableXTEAEncryption();
	setXTEAKey(key);

	/* 
	if (!isProtocolAllowed(version)) {
		std::ostringstream ss;
		ss << "Only clients with protocol " << getClientVersionString(g_game.getClientVersion()) << " allowed!";
		disconnectClient(ss.str(), version);
		return;
	}
	*/

	if (g_game.getGameState() == GAME_STATE_STARTUP) {
		disconnectClient("Gameworld is starting up. Please wait.", version);
		return;
	}

	if (g_game.getGameState() == GAME_STATE_MAINTAIN) {
		disconnectClient("Gameworld is under maintenance.\nPlease re-connect in a while.", version);
		return;
	}

	BanInfo banInfo;
	auto connection = getConnection();
	if (!connection) {
		return;
	}

	if (IOBan::isIpBanned(connection->getIP(), banInfo)) {
		if (banInfo.reason.empty()) {
			banInfo.reason = "(none)";
		}

		std::ostringstream ss;
		ss << "Your IP has been banned until " << formatDateShort(banInfo.expiresAt) << " by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
		disconnectClient(ss.str(), version);
		return;
	}

	uint32_t accountNumber = msg.get<uint32_t>();
	if (!accountNumber) {
		disconnectClient("Invalid account number.", version);
		return;
	}

	
	uint32_t accountNumberOtClientShallow = msg.get<uint32_t>();
	if (!accountNumber || accountNumberOtClientShallow != accountNumber) {
		std::ostringstream ss;
		ss << "Only clients with protocol " << getClientVersionString(g_game.getClientVersion()) << " allowed!";
		disconnectClient(ss.str(), version);
		return; 
	}
	
	std::string password = msg.getString();
	if (password.empty()) {
		disconnectClient("Invalid password.", version);
		return;
	}

	// OTCv8 version detection
	uint16_t otclientV8 = 0;
	uint16_t otcV8StringLength = msg.get<uint16_t>();
	if (otcV8StringLength == 5 && msg.getString(5) == "OTCv8") {
		otclientV8 = msg.get<uint16_t>(); // 253, 260, 261, ...
	}

	auto thisPtr = std::static_pointer_cast<ProtocolLogin>(shared_from_this());
	g_dispatcher.addTask(createTask(std::bind(&ProtocolLogin::getCharacterList, thisPtr, accountNumber, password, version)));
}


bool ProtocolLogin::isProtocolAllowed(uint16_t version)
{
	ClientVersion_t allowedClientVersion = g_game.getClientVersion();

	if (allowedClientVersion == version)
	{
		return true;
	}
	else if (version == 781 && allowedClientVersion == CLIENT_VERSION_780) {
		return true;
	}

	return false;
}