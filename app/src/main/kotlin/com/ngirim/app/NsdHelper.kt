package com.ngirim.app

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import java.net.Inet4Address

/** Registrasi dan penemuan perangkat Ngirim di jaringan lokal (mDNS / NSD). */
class NsdHelper(
    ctx: Context,
    private val onFound: (name: String, host: String, port: Int) -> Unit,
    private val log: (String) -> Unit
) {
    private val nsd = ctx.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val type = "_ngirim._tcp."
    private var reg: NsdManager.RegistrationListener? = null
    private var disc: NsdManager.DiscoveryListener? = null
    private val pending = ArrayDeque<NsdServiceInfo>()
    private var resolving = false
    @Volatile private var selfName = ""

    fun register(name: String, port: Int) {
        unregister()
        val info = NsdServiceInfo().apply {
            serviceName = name
            serviceType = type
            setPort(port)
        }
        val l = object : NsdManager.RegistrationListener {
            override fun onRegistrationFailed(s: NsdServiceInfo, e: Int) { log("ERR NSD gagal ($e) - pakai IP manual") }
            override fun onUnregistrationFailed(s: NsdServiceInfo, e: Int) {}
            override fun onServiceRegistered(s: NsdServiceInfo) { selfName = s.serviceName; log("NSD terdaftar: ${s.serviceName}") }
            override fun onServiceUnregistered(s: NsdServiceInfo) {}
        }
        reg = l
        try { nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, l) } catch (t: Throwable) { log("ERR NSD: ${t.message}") }
    }

    fun unregister() {
        reg?.let { try { nsd.unregisterService(it) } catch (_: Throwable) {} }
        reg = null
        selfName = ""
    }

    fun startDiscovery() {
        stopDiscovery()
        val l = object : NsdManager.DiscoveryListener {
            override fun onStartDiscoveryFailed(t: String, e: Int) { log("ERR pencarian gagal ($e)") }
            override fun onStopDiscoveryFailed(t: String, e: Int) {}
            override fun onDiscoveryStarted(t: String) {}
            override fun onDiscoveryStopped(t: String) {}
            override fun onServiceFound(s: NsdServiceInfo) { if (s.serviceName != selfName) enqueue(s) }
            override fun onServiceLost(s: NsdServiceInfo) {}
        }
        disc = l
        try { nsd.discoverServices(type, NsdManager.PROTOCOL_DNS_SD, l) } catch (t: Throwable) { log("ERR NSD: ${t.message}") }
    }

    fun stopDiscovery() {
        disc?.let { try { nsd.stopServiceDiscovery(it) } catch (_: Throwable) {} }
        disc = null
        synchronized(this) { pending.clear(); resolving = false }
    }

    private fun enqueue(s: NsdServiceInfo) {
        synchronized(this) { pending.addLast(s) }
        next()
    }

    @Suppress("DEPRECATION")
    private fun next() {
        val s: NsdServiceInfo
        synchronized(this) {
            if (resolving) return
            s = pending.removeFirstOrNull() ?: return
            resolving = true
        }
        try {
            nsd.resolveService(s, object : NsdManager.ResolveListener {
                override fun onResolveFailed(i: NsdServiceInfo, e: Int) { done() }
                override fun onServiceResolved(i: NsdServiceInfo) {
                    val h = i.host
                    if (h is Inet4Address) onFound(i.serviceName, h.hostAddress ?: "", i.port)
                    done()
                }
            })
        } catch (t: Throwable) { done() }
    }

    private fun done() { synchronized(this) { resolving = false }; next() }
}
