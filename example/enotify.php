<?php

define('EVENT_TYPE_CHANNEL', 0);

function enotify() {
  return enotify::singleton();
}

class enotify {
  static function make_eventID($eventType, $eventData) {
    return (($eventType << 56) | $eventData);
  }

  static function singleton() {
    if (self::$instance === null) self::$instance = new enotify();
    return self::$instance;
  }

  protected function __construct($_subscribe_port = 9090, $_publish_port = 9091) {
    $this->subscribe_port = $_subscribe_port;
    $this->publish_port = $_publish_port;
  }

  function publish_event($eventType, $eventData, $version = 0) {
    $eventID = self::make_eventID($eventType, $eventData);
    return $this->publish_events(array($eventID => $version));
  }

  function publish_events($event_versions) {
    // who needs json encode? i don't!
    $encoded = array();
    foreach ($event_versions as $eventID => $version) {
      $encoded[] = '\''.intval($eventID).'\':'.intval($version);
    }
    $payload = '{'.implode(',',$encoded).'}';

    $curl = curl_init();
    $opts = array(
      CURLOPT_CONNECTTIMEOUT => 5,
      CURLOPT_RETURNTRANSFER => 1,
      CURLOPT_POSTFIELDS => $payload,
      CURLOPT_URL => 'http://127.0.0.1:'.$this->publish_port.'/notify',
    );
    foreach ($opts as $k => $v) { curl_setopt($curl, $k, $v); }

    $response = curl_exec($curl); 

    $response = preg_replace('/HTTP\/1\.[01] 100.*(.+\r\n)*\r\n/',
                             '',
                             $response);

    if (preg_match('/^HTTP\/1\.. ([^ ]*)/', $response, $matches) &&
        isset($matches[1])) {
      $ret['http_code'] = (int)$matches[1];
      list($headers, $body) = explode("\r\n\r\n", $response, $limit = 2);
      $ret['headers'] = $headers;
      $ret['body'] = $body;
    }

    //return (strpos($response, '200 OK') !== false);
    return $response;
  }

  function get_subscribe_url() {
    return '/subscribe';
  }

  protected $publish_port;
  protected $subscribe_port;

  private static $instance;
}

