package com.fyewebcam

import java.net.Inet4Address
import java.net.NetworkInterface

object NetUtils {
    /** IPv4 addresses the PC can reach this phone on (Wi-Fi, hotspot, Ethernet). */
    fun localIpv4Addresses(): List<String> = try {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.inetAddresses.toList() }
            .filterIsInstance<Inet4Address>()
            .filter { it.isSiteLocalAddress }
            .map { it.hostAddress ?: "" }
            .filter { it.isNotEmpty() }
            .distinct()
    } catch (e: Exception) {
        emptyList()
    }
}
