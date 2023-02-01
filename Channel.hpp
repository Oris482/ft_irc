#pragma once

#ifndef CHANNEL_HPP
# define CHANNEL_HPP

# include <map>
# include <set>
# include <vector>
# include "User.hpp"
# define UNDEFINED_FD -1
# define DEFAULT_PART_MESSAGE " leaved channel."
# define NEW_OPERATOR_MESSAGE " is new channel operator."

using namespace std;

class Message;

class Channel {
    private:
		string _name;
		map<int, User *> _userList;
		set<int> _operList;

        Channel(void);
        Channel(const Channel& channel);
        Channel& operator=(const Channel& channel);

    public:
        Channel(const string& name);
        ~Channel();

        const string& getName(void) const;
        const vector<string> getUserList(void) const;

        void addUser(int clientFd, User *user);
        int deleteUser(int clientFd);
        User* findUser(const int clientFd);
        bool isUserOper(int clientFd) const;
        void broadcast(const string& msg, int ignoreFd = UNDEFINED_FD) const;
        void broadcast(const Message& msg, int ignoreFd = UNDEFINED_FD) const;
};

#endif
