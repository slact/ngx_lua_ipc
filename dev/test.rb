#!/usr/bin/ruby
require 'minitest'
require 'minitest/reporters'
require "minitest/autorun"
Minitest::Reporters.use! [Minitest::Reporters::SpecReporter.new(:color => true)]
require 'securerandom'

SERVER=ENV["NGINX_SERVER"] || "127.0.0.1"
PORT=ENV["NGINX_PORT"] || "8082"

puts "Server at #{url}"

class PubSubTest <  Minitest::Test
  def test_nothing
  end
end


