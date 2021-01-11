/* 
 * This file is part of the HDDP Switch distribution (https://github.com/gistnetserv-uah/HDDP).
 * Copyright (c) 2020.
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

package DHTapp;

import org.onlab.packet.MacAddress;
import org.onlab.packet.VlanId;
import org.onosproject.net.*;
import org.onosproject.net.config.basics.BasicHostConfig;
import org.onosproject.net.config.basics.BasicLinkConfig;
import org.onosproject.net.host.DefaultHostDescription;
import org.onosproject.net.host.HostProvider;
import org.onosproject.net.host.HostProviderService;
import org.onosproject.net.host.HostService;
import org.onosproject.net.provider.ProviderId;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.net.URI;
import java.util.Collections;

import static org.onosproject.net.DeviceId.deviceId;
import static org.onosproject.net.PortNumber.portNumber;

/**
 * @brief Provider de host para DHT
 * @author Joaquin Alvarez Horcajo
 */

public class DHTproviderhost implements HostProvider {

    private final Logger log = LoggerFactory.getLogger(getClass());
    public short TYPE_SENSOR = 3;
    /** Tipos de sensores admitidos*/


    public static ProviderId PID;

    public DHTproviderhost(ProviderId pid){
        PID = pid;
    }

    public void Createhost(HostProviderService hps, MacAddress Mac, DeviceId iddevice, PortNumber portNode,
            short type_device, String TYPE_SENSORS[]){
        try{
            HostId hostId = HostId.hostId(Mac);
            HostLocation location = new HostLocation(new ConnectPoint(iddevice, portNode), 0);

            DefaultHostDescription desc =
                    new DefaultHostDescription(Mac, VlanId.vlanId("None"), location, Collections.emptySet(),
                            true, DefaultAnnotations.builder()
                            .set("name", TYPE_SENSORS[type_device-TYPE_SENSOR]+" Sensor\n"+hostId.toString())
                            .build());
            hps.hostDetected(hostId, desc, true);
        }catch (Exception e){
            log.info("Algo no fue bien al crear los putos host!!! "+e.getMessage());
        }

    }

    public boolean hostexist(HostService hsrv, MacAddress Mac)
    {
        if (hsrv.getHost(HostId.hostId(Mac))!=null){
            return true;
        }
        return false;
    }

    public MacAddress long2mac(String Mac){
        String Mac_add;

        if (Mac.length()<12){
            /* Necesitamos meter 0 por delante */
            for (int pos = Mac.length(); pos < 12; pos++)
                Mac = "0" + Mac;
        }
        /* Normalizamos a MAC*/
        Mac_add = String.valueOf(Mac.charAt(0))+Mac.charAt(1)+":"+Mac.charAt(2)+Mac.charAt(3)+":"+
                Mac.charAt(4)+Mac.charAt(5)+":"+Mac.charAt(6)+Mac.charAt(7)+":"+
                Mac.charAt(8)+Mac.charAt(9)+":"+Mac.charAt(10)+Mac.charAt(11);
        log.info(Mac_add);
        return MacAddress.valueOf(Mac_add);
    }

    public void Movehost(HostService hsv, HostProviderService hps, MacAddress Mac, DeviceId iddevice,
                         PortNumber portNode, short type_device, String TYPE_SENSORS[]){
        //MacAddress mac = MacAddress.valueOf(Mac);
        HostId hostId = HostId.hostId(Mac);
        /** Eliminamos el host */
        /*if(hostexist(hsv, Mac))
            hps.hostVanished(hostId);
        /** Lo volvemos a crear en el nuevo punto*/
        Createhost(hps, Mac,iddevice, portNode, type_device, TYPE_SENSORS);
    }

    public boolean checkhost(HostService hsv, HostProviderService hps, DHTpacket Packet_dht, short modeDHT, String TYPE_SENSORS[]){
        short type_device[] = Packet_dht.getTypedevices();
        long idmacs[] = Packet_dht.getidmacdevices();
        int port_in_gateway[] = Packet_dht.getoutports(); /** En el caso de los host es el mismo */

        for (int pos = 0; pos < Packet_dht.getNumDevices(); pos++){
            /** Solo se permite crear sensores como host en el modo 0 del protocolo */
            if (type_device[pos] >= TYPE_SENSOR && modeDHT == 0){

                /** Debemos actualizar los dispositivos siempre */
                try{
                    if(!hostexist(hsv, long2mac(Long.toHexString(idmacs[pos])))){
                        Createhost(hps, long2mac(Long.toHexString(idmacs[pos])),
                                deviceId(URI.create("sw:"+Long.toHexString(idmacs[pos+1]))),
                                PortNumber.portNumber(port_in_gateway[pos+1]),
                                type_device[pos], TYPE_SENSORS);
                    }
                    else{
                        Movehost(hsv, hps, long2mac(Long.toHexString(idmacs[pos])),
                                deviceId(URI.create("sw:"+Long.toHexString(idmacs[pos + 1]))),
                                portNumber(port_in_gateway[pos+1]),type_device[pos], TYPE_SENSORS);
                    }

                }catch (Exception e){
                    log.error("DHTapp ERROR al crear nuevos host : "+ e.getMessage());
                    return false;
                }
            }
        }
        return true;
    }

    @Override
    public void triggerProbe(Host host) {

    }

    @Override
    public ProviderId id() {
        return PID;
    }
}
