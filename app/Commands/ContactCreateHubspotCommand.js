import axios from "axios";

export default {

    name: 'hubspot-create',
    description: 'Contact Hubspot',
    arguments: {

    },

    handle: async function () {
        const hubspotToken = process.env.HUBSPOT_API_TOKEN;
        if (!hubspotToken) {
            console.error('HUBSPOT_API_TOKEN is not set in the environment variables.');
            return;
        }



        const url = `https://api.hubapi.com/crm/v3/objects/contacts`;

        console.log(url);

        const response = await axios.post(url, {
            "properties": {
                "firstname": "John",
                "lastname": "Doe",
                "email": "john.doe@example.com"
            }
        }, {
            headers: {
                'Authorization': `Bearer ${hubspotToken}`
            }
        });

        console.log(response.data);
    }
}