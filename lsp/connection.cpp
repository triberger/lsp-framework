#include "connection.h"

#include <cassert>
#include <charconv>
#include <string_view>
#include <lsp/util/str.h>

namespace lsp{

Connection::Connection(std::istream& in, std::ostream& out) : m_in{in},
                                                              m_out{out}{}

std::variant<jsonrpc::MessagePtr, std::vector<jsonrpc::MessagePtr>> Connection::receiveMessage(){
	std::lock_guard lock{m_writeMutex};

	if(m_in.peek() == std::char_traits<char>::eof())
		throw ConnectionError{"Connection lost"};

	auto header = readMessageHeader();

	std::string content;
	content.resize(header.contentLength);
	m_in.read(&content[0], static_cast<std::streamsize>(header.contentLength));

	// Verify only after reading the entire message so no partial unread message is left in the stream

	std::string_view contentType{header.contentType};

	if(!contentType.starts_with("application/vscode-jsonrpc"))
		throw ProtocolError{"Unsupported or invalid content type: " + header.contentType};

	const std::string_view charsetKey{"charset="};
	if(auto idx = contentType.find(charsetKey); idx != std::string_view::npos){
		auto charset = contentType.substr(idx + charsetKey.size());
		charset = util::str::trimView(charset.substr(0, charset.find(';')));

		if(charset != "utf-8" && charset != "utf8")
			throw ProtocolError{"Unsupported or invalid character encoding: " + std::string{charset}};
	}

	return jsonrpc::messageFromJson(json::parse(content));
}

void Connection::sendMessage(const jsonrpc::Message& message){
	writeJsonMessage(message.toJson());
}

void Connection::sendMessageBatch(const jsonrpc::MessageBatch& batch){
	json::Any content = json::Array{};
	auto& array = content.get<json::Array>();
	array.reserve(batch.size());

	for(const auto& m : batch)
		array.push_back(m->toJson());

	writeJsonMessage(content);
}

void Connection::writeJsonMessage(const json::Any& content){
	std::string contentStr = json::stringify(content);
	assert(!contentStr.empty());
	MessageHeader header{contentStr.size()};
	writeMessage(contentStr);
}

void Connection::writeMessage(const std::string& content){
	std::lock_guard lock{m_writeMutex};
	MessageHeader header{content.size()};
	writeMessageHeader(header);
	m_out.write(content.data(), static_cast<std::streamsize>(content.size()));
	m_out.flush();
}

void Connection::writeMessageHeader(const MessageHeader& header){
	assert(header.contentLength > 0);
	assert(!header.contentType.empty());
	std::string headerStr = "Content-Length: " + std::to_string(header.contentLength) + "\r\n\r\n";
	m_out.write(headerStr.data(), static_cast<std::streamsize>(headerStr.length()));
}

Connection::MessageHeader Connection::readMessageHeader(){
	MessageHeader header;

	while(m_in.peek() != '\r')
		readNextMessageHeaderField(header);

	m_in.get(); // \r

	if(m_in.peek() != '\n')
		throw ProtocolError{"Invalid message header format"};

	m_in.get(); // \n

	return header;
}

void Connection::readNextMessageHeaderField(MessageHeader& header){
	if(m_in.peek() == std::char_traits<char>::eof())
		throw ConnectionError{"Connection lost"};

	std::string lineData;
	std::getline(m_in, lineData); // This also consumes the newline so it's only necessary to check for one \r\n before the content

	std::string_view line{lineData};
	std::size_t separatorIdx = line.find(':');

	if(separatorIdx != std::string_view::npos){
		std::string_view key = util::str::trimView(line.substr(0, separatorIdx));
		std::string_view value = util::str::trimView(line.substr(separatorIdx + 1));

		if(key == "Content-Length")
			std::from_chars(value.data(), value.data() + value.size(), header.contentLength);
		else if(key == "Content-Type")
			header.contentType = std::string{value.data(), value.size()};
	}
}

} // namespace lsp