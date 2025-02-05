#include "Command.hpp"

#include "Server.hpp"
#include "User.hpp"
#include "Channel.hpp"
#include "Message.hpp"

#include "FormatValidator.hpp"
#include "Reply.hpp"
#include "CommonValue.hpp"

/**
 * @brief Construct a new Command:: Command object
 * 
 * @param server Save server class reference for execute commands
 */
Command::Command(Server& server): _server(server) {
	_commands.insert(make_pair("PRIVMSG", &Command::cmdPrivmsg));
	_commands.insert(make_pair("JOIN", &Command::cmdJoin));
	_commands.insert(make_pair("PART", &Command::cmdPart));
	_commands.insert(make_pair("PASS", &Command::cmdPass));
	_commands.insert(make_pair("NICK", &Command::cmdNick));
	_commands.insert(make_pair("USER", &Command::cmdUser));
	_commands.insert(make_pair("PING", &Command::cmdPing));
	_commands.insert(make_pair("QUIT", &Command::cmdQuit));
	_commands.insert(make_pair("KICK", &Command::cmdKick));
	_commands.insert(make_pair("NOTICE", &Command::cmdNotice));
}

/**
 * @brief Destroy the Command:: Command object
 */
Command::~Command() {
	_commands.clear();
}

/**
 * @brief Midleware of the IRC commands.
 * 	Check prefix and command and excute correct function.
 * 	Return value is determined by each command function.
 * @param user User class pointer. Message sender.
 * @param msg Message class reference. It must be parsed by Message constructor before passed.
 * @return true : Keep going //
 * @return false : Notify the server that buffer checks are no longer needed
 */
bool Command::run(User *user, const Message& msg) {
	const string& prefix = msg.getPrefix();
	const string& cmd = msg.getCommand();

	if (!prefix.empty() && prefix != user->getNickname()) return true;
	if (!user->getAuth() && isCommandNeedAuth(cmd)) return true;

	try {
		return (this->*_commands.at(cmd))(user, msg);
	} catch (out_of_range &e) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_UNKNOWNCOMMAND << user->getNickname() << cmd << ERR_UNKNOWNCOMMAND_MSG);
	}
	return true;
}

/**
 * @brief PRIVMSG(IRC command) : Send message to user(s)/channel(s)
 */
bool Command::cmdPrivmsg(User *user, const Message& msg) {
    if (msg.paramSize() < 2) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NORECIPIENT << user->getNickname() << ERR_NORECIPIENT_MSG << "(PRIVMSG)");
		return true;
	}

    const vector<string> targetList = Message::split(msg.getParams()[0], ',');
    for (vector<string>::const_iterator it = targetList.begin(); it != targetList.end(); ++it) {
        string targetName = *it;
        if (targetName[0] == '#') {
            Channel *targetChannel = _server.findChannelByName(targetName);

            if (targetChannel == NULL) {
				user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NOSUCHNICK << user->getNickname() << targetName << ERR_NOSUCHNICK_MSG);
				continue;
			}
            targetChannel->broadcast(Message() << ":" << user->getSource() << msg.getCommand() << targetChannel->getName() << ":" << msg.getParams()[1], user->getFd());
			if (msg.getParams()[1][0] == '!') targetChannel->executeBot(msg.getParams()[1]);
        } else {
            User *targetUser;

            targetUser = _server.findClientByNickname(targetName);
            if (targetUser == NULL) {
				user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NOSUCHNICK << user->getNickname() << targetName << ERR_NOSUCHNICK_MSG);
				continue;
			}
            targetUser->addToReplyBuffer(Message() << ":" << user->getSource() << msg.getCommand() << targetUser->getNickname() << ":" << msg.getParams()[1]);
        }
    }
	return true;
}

/**
 * @brief JOIN(IRC command) : JOIN to channel(s). 
 * 	If there is no channel with the requested name, create a new channel and join.
 * 	A parameter of 0 leaves all participating channels.
 */
bool Command::cmdJoin(User *user, const Message& msg) {
    if (msg.paramSize() == 0) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NEEDMOREPARAMS << user->getNickname() << msg.getCommand() << ERR_NEEDMOREPARAMS_MSG);
		return true;
	}
    
    const vector<string> targetList = Message::split(msg.getParams()[0], ',');
	// A parameter of 0 leaves all participating channels.
    if (targetList.size() == 1 && targetList[0] == "0") {
        vector<string> removeWaitingChannels;
		const vector<Channel *>& chs = user->getMyAllChannel();
        for (vector<Channel *>::const_iterator it = chs.begin(); it != chs.end(); ++it) {
			Channel *targetChannel = *it;
			
            const int remainUserOfChannel = targetChannel->deleteUser(user->getFd());
			user->addToReplyBuffer(Message() << ":" << user->getSource() << "PART" << targetChannel->getName());
			targetChannel->broadcast(Message() << ":" << user->getSource() << "PART" << targetChannel->getName());
            if (remainUserOfChannel == 0) removeWaitingChannels.push_back(targetChannel->getName());
        }
		user->clearMyChannelList();
        for (vector<string>::iterator it = removeWaitingChannels.begin(); it != removeWaitingChannels.end(); ++it) {
            _server.deleteChannel(*it);
        }
        return true;
    }

	// General parameters
    for (vector<string>::const_iterator it = targetList.begin(); it != targetList.end(); ++it) {
        string targetChannelName = *it;
        if (targetChannelName[0] != '#') {
			user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NOSUCHCHANNEL << user->getNickname() << targetChannelName << ERR_NOSUCHCHANNEL_MSG);
			continue;
		}
		if (targetChannelName.length() > 31) targetChannelName.erase(MAX_CHANNELNAME_LEN);
		if (!FormatValidator::isValidChannelname(targetChannelName)) {
			user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_ERRONEUSCHANNELNAME << user->getNickname() << targetChannelName << ERR_ERRONEUSCHANNELNAME_MSG);
			continue;
		}

        Channel *targetChannel;

        targetChannel = _server.findChannelByName(targetChannelName);
		// Channel with that name does not exist
        if (targetChannel == NULL) {
            targetChannel = _server.addChannel(targetChannelName);
			if (targetChannel == NULL) {
				user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_UNAVAILRESOURCE << targetChannelName << ERR_UNAVAILRESOURCE_MSG);
				return true;
			}
		// User is already participating in that channel
        } else if (targetChannel->findUser(user->getFd()) != NULL) continue;
		
		// Join the user on that channel
        targetChannel->addUser(user->getFd(), user);
		user->addToMyChannelList(targetChannel);
		Message replyMsg[3];
		replyMsg[0] << ":" << user->getSource() << msg.getCommand() << ":" << targetChannelName;
		replyMsg[1] << ":" << SERVER_HOSTNAME << RPL_NAMREPLY << user->getNickname() << "=" << targetChannelName << ":";
		vector<string> targetChannelUserList = targetChannel->getUserList();
		for (vector<string>::iterator it = targetChannelUserList.begin(); it != targetChannelUserList.end(); ++it) {
			replyMsg[1] << *it;
		}
		replyMsg[2] << ":" << SERVER_HOSTNAME << RPL_ENDOFNAMES << user->getNickname() << targetChannelName << RPL_ENDOFNAMES_MSG;
		targetChannel->broadcast(replyMsg[0]);
		user->addToReplyBuffer(replyMsg[1]);		
		user->addToReplyBuffer(replyMsg[2]);
    }
	return true;
}

/**
 * @brief PART(IRC command) : PART from channel(s). 
 */
bool Command::cmdPart(User *user, const Message& msg) {
	if (msg.paramSize() < 1) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NEEDMOREPARAMS << user->getNickname() << msg.getCommand() << ERR_NEEDMOREPARAMS_MSG);
		return true;
	}

    string partNotiMessage;
    if (msg.paramSize() >= 2) {
		partNotiMessage.append(":");
		partNotiMessage.append(msg.getParams()[1]);
	}

    const vector<string> targetList = Message::split(msg.getParams()[0], ',');
    for (vector<string>::const_iterator it = targetList.begin(); it != targetList.end(); ++it) {
        string targetChannelName = *it;

        Channel *targetChannel;
        targetChannel = _server.findChannelByName(targetChannelName);

        if (targetChannel == NULL) {
			user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NOSUCHCHANNEL << user->getNickname() << targetChannelName << ERR_NOSUCHCHANNEL_MSG);
			continue;
		}
		if (targetChannel->findUser(user->getFd()) == NULL) {
			user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NOTONCHANNEL << user->getNickname() << targetChannelName << ERR_NOTONCHANNEL_MSG);
			continue;
		}
        const int remainUserOfChannel = targetChannel->deleteUser(user->getFd());
		user->deleteFromMyChannelList(targetChannel);
		user->addToReplyBuffer(Message() << ":" << user->getSource() << "PART" << targetChannelName << partNotiMessage);
		targetChannel->broadcast(Message() << ":" << user->getSource() << "PART" << targetChannelName << partNotiMessage);
        if (remainUserOfChannel == 0) _server.deleteChannel(targetChannelName);
    }
	return true;
}

/**
 * @brief PASS(IRC command) :  Passed server password by user. It will be checked after both of NICK and USER 
 */
bool Command::cmdPass(User *user, const Message& msg) {
	if (msg.paramSize() < 1) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NEEDMOREPARAMS << user->getNickname() << msg.getCommand() << ERR_NEEDMOREPARAMS_MSG);
		return true;
	}
	if (user->getAuth()) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_ALREADYREGISTERED << user->getNickname() << ERR_ALREADYREGISTERED_MSG);
		return true;
	}
	user->setPassword(msg.getParams()[0]);
	return true;
}

/**
 * @brief NICK(IRC command) :  Request to set/change nickname by user.
 */
bool Command::cmdNick(User *user, const Message& msg) {
	if (msg.paramSize() < 1) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NEEDMOREPARAMS << user->getNickname() << msg.getCommand() << ERR_NEEDMOREPARAMS_MSG);
		return true;
	}
	string requestNickname = msg.getParams()[0];
	const string originNickname = user->getNickname();

	if (requestNickname.length() == 0) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NONICKNAMEGIVEN << originNickname << ERR_NONICKNAMEGIVEN_MSG);
		return true;
	}

	if (_server.findClientByNickname(requestNickname) != NULL) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NICKNAMEINUSE << originNickname << requestNickname << ERR_NICKNAMEINUSE_MSG);
		return true;
	}
	
	if (requestNickname.length() > MAX_NICKNAME_LEN) requestNickname = requestNickname.erase(MAX_NICKNAME_LEN);
	if (!FormatValidator::isValidNickname(requestNickname)) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_ERRONEUSNICKNAME << requestNickname << ERR_ERRONEUSNICKNAME_MSG);
		return true;
	}
	user->setNickname(requestNickname);
	if (!user->getAuth() && !user->getUsername().empty()) {
		if (_server.checkPassword(user->getPassword())) {
			user->setAuth();
			user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << RPL_WELCOME << user->getNickname() << ":Welcome to the" << SERVER_HOSTNAME <<  "Network" << requestNickname);
			return true;
		} else {
			user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_PASSWDMISMATCH << user->getNickname() << ERR_PASSWDMISMATCH_MSG);
			_server.disconnectClient(user->getFd());
			return false;
		}
	}
	if (user->getMyAllChannel().empty()) user->addToReplyBuffer(Message() << ":" << originNickname << msg.getCommand() << requestNickname);
	else user->broadcastToMyChannels(Message() << ":" << originNickname << msg.getCommand() << requestNickname);
	return true;
}

/**
 * @brief USER(IRC command) :  Register user info passed by user.
 */
bool Command::cmdUser(User *user, const Message& msg) {
	if (msg.paramSize() < 4) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NEEDMOREPARAMS << user->getNickname() << msg.getCommand() << ERR_NEEDMOREPARAMS_MSG);
		return true;
	}
	
	if (user->getAuth()) { // 이미 auth되어 있는 user
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_ALREADYREGISTERED << user->getNickname() << ERR_ALREADYREGISTERED_MSG);
		return true;
	}
	
	const string requestUserNickname = msg.getParams()[0]; //username
	
	if (requestUserNickname.length() == 0) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NEEDMOREPARAMS << user->getNickname() << msg.getCommand() << ERR_NEEDMOREPARAMS_MSG);
		return true;
	}
	
	user->setUsername(requestUserNickname);
	if (user->getNickname() != "*") {
		if (_server.checkPassword(user->getPassword())) {
			user->setAuth();
			user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << RPL_WELCOME << user->getNickname() << ":Welcome to the" << SERVER_HOSTNAME <<  "Network" << requestUserNickname);
			return true;
		}
		else {
			user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_PASSWDMISMATCH << user->getNickname() << ERR_PASSWDMISMATCH_MSG);
			_server.disconnectClient(user->getFd());
			return false;
		}
	}
	return true;
}

/**
 * @brief PING(IRC command) : Signal for maintaining connection. 
 */
bool Command::cmdPing(User *user, const Message& msg) {
	if (msg.paramSize() < 1) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NEEDMOREPARAMS << user->getNickname() << msg.getCommand() << ERR_NEEDMOREPARAMS_MSG);
		return true;
	}

	if (msg.getParams()[0].empty()) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NOORIGIN << user->getNickname() << ERR_NOORIGIN_MSG);
		return true;
	}

	user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << "PONG" << SERVER_HOSTNAME << msg.getParams()[0]);
	return true;
}

/**
 * @brief QUIT(IRC command) :  User leaves the server.
 */
bool Command::cmdQuit(User *user, const Message& msg) {
	string reason = ":Quit:";
	if (msg.paramSize() == 1) reason += msg.getParams()[0];
	else reason += "leaving";

	user->clearCmdBuffer();
	user->setReplyBuffer("\r\nERROR :Closing Link: " + user->getHost() + " " + reason + "\r\n");
	user->broadcastToMyChannels(Message() << ":" << user->getSource() << msg.getCommand() << reason, user->getFd());
	user->setIsQuiting();
	return false;
}

/**
 * @brief KICK(IRC command) :  Channel operator kick user from channel.
 */
bool Command::cmdKick(User *user, const Message& msg) {
	if (msg.paramSize() < 2)
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NEEDMOREPARAMS << user->getNickname() << msg.getCommand() << ERR_NEEDMOREPARAMS_MSG);
	
	// 해당 channel이 존재하는 지 check
	Channel *targetChannel = _server.findChannelByName(msg.getParams()[0]);
	if (targetChannel == NULL) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NOSUCHCHANNEL << user->getNickname() << msg.getParams()[0] << ERR_NOSUCHCHANNEL_MSG);
		return true;
	}
	
	// User가 channel에 있는 지 check
	if (targetChannel->findUser(user->getFd()) == NULL) {\
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NOTONCHANNEL << user->getNickname() << msg.getParams()[0] << ERR_NOTONCHANNEL_MSG);
		return true;
	}

	// User가 해당 channel의 operator인지
	if (targetChannel->isUserOper(user->getFd()) == false) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_CHANOPRIVSNEEDED << user->getNickname() << msg.getParams()[0] << ERR_CHANOPRIVSNEEDED_MSG);
		return true;
	}

	// iteration
	const vector<string> targetUsers = Message::split(msg.getParams()[1], ',');
	string reason;
	if (msg.paramSize() >= 3) {
		reason.append(":");
		reason.append(msg.getParams()[2]);
	}

	for (vector<string>::const_iterator it = targetUsers.begin(); it != targetUsers.end(); ++it) {
		// target User가 channel에 존재하는지
		User *targetUser = targetChannel->findUser(*it);

		if (targetUser == NULL) {
			user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_USERNOTINCHANNEL << user->getNickname() << *it << msg.getParams()[0] << ERR_USERNOTINCHANNEL_MSG);
			continue;
		}

		// 존재하면 Kick (그 channel에 deleteUser)
		targetChannel->broadcast(Message() << ":" << user->getSource() << msg.getCommand() << msg.getParams()[0] << *it << reason);
		const int remainUsers = targetChannel->deleteUser(targetUser->getFd());
		if (remainUsers == 0) _server.deleteChannel(targetChannel->getName());
		targetUser->deleteFromMyChannelList(targetChannel);
	}
	return true;
}

/**
 * @brief NOTICE(IRC command) :  Notice to user(s)/channel(s).
 */
bool Command::cmdNotice(User *user, const Message& msg) {
	if (msg.paramSize() == 0) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NORECIPIENT << user->getNickname() << ERR_NORECIPIENT_MSG << "(NOTICE)");
		return true;
	}
	if (msg.paramSize() == 1) {
		user->addToReplyBuffer(Message() << ":" << SERVER_HOSTNAME << ERR_NOTEXTTOSEND << user->getNickname() << msg.getCommand() << ERR_NOTEXTTOSEND_MSG);
		return true;
	}

    const vector<string> targetList = Message::split(msg.getParams()[0], ',');
    for (vector<string>::const_iterator it = targetList.begin(); it != targetList.end(); ++it) {
        string targetName = *it;
        if (targetName[0] == '#') {
            Channel *targetChannel;

            targetChannel = _server.findChannelByName(targetName);
            if (targetChannel == NULL) continue;
            targetChannel->broadcast(Message() << ":" << user->getSource() << msg.getCommand() << targetName << ":" << msg.getParams()[1]);
        } else {
            User *targetUser;

            targetUser = _server.findClientByNickname(targetName);
            if (targetUser == NULL) continue;
            targetUser->addToReplyBuffer(Message() << ":" << user->getSource() << msg.getCommand() << targetName << ":" << msg.getParams()[1]);
        }
    }
	return true;
}

bool Command::isCommandNeedAuth(const string& cmd) {
	if (cmd == "PASS" || cmd == "NICK" || cmd == "USER" || cmd == "PING" || cmd == "QUIT") return false;

	return true;
}
